/*-------------------------------------------------------------------------
 *
 * copy.c
 *		Implements the COPY utility command
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/copy.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>
#include <unistd.h>
#include <sys/stat.h>

#include "access/sysattr.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/pg_authid.h"
#include "commands/copy.h"
#include "commands/defrem.h"
#include "executor/executor.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "optimizer/optimizer.h"
#include "parser/parse_coerce.h"
#include "parser/parse_collate.h"
#include "parser/parse_expr.h"
#include "parser/parse_relation.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/rls.h"

/*
 *	 DoCopy executes the SQL COPY statement
 *
 * Either unload or reload contents of table <relation>, depending on <from>.
 * (<from> = true means we are inserting into the table.)  In the "TO" case
 * we also support copying the output of an arbitrary SELECT, INSERT, UPDATE
 * or DELETE query.
 *
 * If <pipe> is false, transfer is between the table and the file named
 * <filename>.  Otherwise, transfer is between the table and our regular
 * input/output stream. The latter could be either stdin/stdout or a
 * socket, depending on whether we're running under Postmaster control.
 *
 * Do not allow a Postgres user without the 'pg_read_server_files' or
 * 'pg_write_server_files' role to read from or write to a file.
 *
 * Do not allow the copy if user doesn't have proper permission to access
 * the table or the specifically requested columns.
 */
void
DoCopy(ParseState *pstate, const CopyStmt *stmt,
	   int stmt_location, int stmt_len,
	   uint64 *processed)
{
	bool		is_from = stmt->is_from;
	bool		pipe = (stmt->filename == NULL);
	Relation	rel;
	Oid			relid;
	RawStmt    *query = NULL;
	Node	   *whereClause = NULL;

	/*
	 * Disallow COPY to/from file or program except to users with the
	 * appropriate role.
	 */
	if (!pipe)
	{
		if (stmt->is_program)
		{
			if (!has_privs_of_role(GetUserId(), ROLE_PG_EXECUTE_SERVER_PROGRAM))
				ereport(ERROR,
						(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						 errmsg("permission denied to COPY to or from an external program"),
						 errdetail("Only roles with privileges of the \"%s\" role may COPY to or from an external program.",
								   "pg_execute_server_program"),
						 errhint("Anyone can COPY to stdout or from stdin. "
								 "psql's \\copy command also works for anyone.")));
		}
		else
		{
			if (is_from && !has_privs_of_role(GetUserId(), ROLE_PG_READ_SERVER_FILES))
				ereport(ERROR,
						(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						 errmsg("permission denied to COPY from a file"),
						 errdetail("Only roles with privileges of the \"%s\" role may COPY from a file.",
								   "pg_read_server_files"),
						 errhint("Anyone can COPY to stdout or from stdin. "
								 "psql's \\copy command also works for anyone.")));

			if (!is_from && !has_privs_of_role(GetUserId(), ROLE_PG_WRITE_SERVER_FILES))
				ereport(ERROR,
						(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						 errmsg("permission denied to COPY to a file"),
						 errdetail("Only roles with privileges of the \"%s\" role may COPY to a file.",
								   "pg_write_server_files"),
						 errhint("Anyone can COPY to stdout or from stdin. "
								 "psql's \\copy command also works for anyone.")));
		}
	}

	if (stmt->relation)
	{
		LOCKMODE	lockmode = is_from ? RowExclusiveLock : AccessShareLock;
		ParseNamespaceItem *nsitem;
		RTEPermissionInfo *perminfo;
		TupleDesc	tupDesc;
		List	   *attnums;
		ListCell   *cur;

		Assert(!stmt->query);

		/* Open and lock the relation, using the appropriate lock type. */
		rel = table_openrv(stmt->relation, lockmode);

		relid = RelationGetRelid(rel);

		nsitem = addRangeTableEntryForRelation(pstate, rel, lockmode,
											   NULL, false, false);

		perminfo = nsitem->p_perminfo;
		perminfo->requiredPerms = (is_from ? ACL_INSERT : ACL_SELECT);

		if (stmt->whereClause)
		{
			/* add nsitem to query namespace */
			addNSItemToQuery(pstate, nsitem, false, true, true);

			/* Transform the raw expression tree */
			whereClause = transformExpr(pstate, stmt->whereClause, EXPR_KIND_COPY_WHERE);

			/* Make sure it yields a boolean result. */
			whereClause = coerce_to_boolean(pstate, whereClause, "WHERE");

			/* we have to fix its collations too */
			assign_expr_collations(pstate, whereClause);

			whereClause = eval_const_expressions(NULL, whereClause);

			whereClause = (Node *) canonicalize_qual((Expr *) whereClause, false);
			whereClause = (Node *) make_ands_implicit((Expr *) whereClause);
		}

		tupDesc = RelationGetDescr(rel);
		attnums = CopyGetAttnums(tupDesc, rel, stmt->attlist);
		foreach(cur, attnums)
		{
			int			attno;
			Bitmapset **bms;

			attno = lfirst_int(cur) - FirstLowInvalidHeapAttributeNumber;
			bms = is_from ? &perminfo->insertedCols : &perminfo->selectedCols;

			*bms = bms_add_member(*bms, attno);
		}
		ExecCheckPermissions(pstate->p_rtable, list_make1(perminfo), true);

		/*
		 * Permission check for row security policies.
		 *
		 * check_enable_rls will ereport(ERROR) if the user has requested
		 * something invalid and will otherwise indicate if we should enable
		 * RLS (returns RLS_ENABLED) or not for this COPY statement.
		 *
		 * If the relation has a row security policy and we are to apply it
		 * then perform a "query" copy and allow the normal query processing
		 * to handle the policies.
		 *
		 * If RLS is not enabled for this, then just fall through to the
		 * normal non-filtering relation handling.
		 */
		if (check_enable_rls(relid, InvalidOid, false) == RLS_ENABLED)
		{
			SelectStmt *select;
			ColumnRef  *cr;
			ResTarget  *target;
			RangeVar   *from;
			List	   *targetList = NIL;

			if (is_from)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("COPY FROM not supported with row-level security"),
						 errhint("Use INSERT statements instead.")));

			/*
			 * Build target list
			 *
			 * If no columns are specified in the attribute list of the COPY
			 * command, then the target list is 'all' columns. Therefore, '*'
			 * should be used as the target list for the resulting SELECT
			 * statement.
			 *
			 * In the case that columns are specified in the attribute list,
			 * create a ColumnRef and ResTarget for each column and add them
			 * to the target list for the resulting SELECT statement.
			 */
			if (!stmt->attlist)
			{
				cr = makeNode(ColumnRef);
				cr->fields = list_make1(makeNode(A_Star));
				cr->location = -1;

				target = makeNode(ResTarget);
				target->name = NULL;
				target->indirection = NIL;
				target->val = (Node *) cr;
				target->location = -1;

				targetList = list_make1(target);
			}
			else
			{
				ListCell   *lc;

				foreach(lc, stmt->attlist)
				{
					/*
					 * Build the ColumnRef for each column.  The ColumnRef
					 * 'fields' property is a String node that corresponds to
					 * the column name respectively.
					 */
					cr = makeNode(ColumnRef);
					cr->fields = list_make1(lfirst(lc));
					cr->location = -1;

					/* Build the ResTarget and add the ColumnRef to it. */
					target = makeNode(ResTarget);
					target->name = NULL;
					target->indirection = NIL;
					target->val = (Node *) cr;
					target->location = -1;

					/* Add each column to the SELECT statement's target list */
					targetList = lappend(targetList, target);
				}
			}

			/*
			 * Build RangeVar for from clause, fully qualified based on the
			 * relation which we have opened and locked.  Use "ONLY" so that
			 * COPY retrieves rows from only the target table not any
			 * inheritance children, the same as when RLS doesn't apply.
			 */
			from = makeRangeVar(get_namespace_name(RelationGetNamespace(rel)),
								pstrdup(RelationGetRelationName(rel)),
								-1);
			from->inh = false;	/* apply ONLY */

			/* Build query */
			select = makeNode(SelectStmt);
			select->targetList = targetList;
			select->fromClause = list_make1(from);

			query = makeNode(RawStmt);
			query->stmt = (Node *) select;
			query->stmt_location = stmt_location;
			query->stmt_len = stmt_len;

			/*
			 * Close the relation for now, but keep the lock on it to prevent
			 * changes between now and when we start the query-based COPY.
			 *
			 * We'll reopen it later as part of the query-based COPY.
			 */
			table_close(rel, NoLock);
			rel = NULL;
		}
	}
	else
	{
		Assert(stmt->query);

		query = makeNode(RawStmt);
		query->stmt = stmt->query;
		query->stmt_location = stmt_location;
		query->stmt_len = stmt_len;

		relid = InvalidOid;
		rel = NULL;
	}

	if (is_from)
	{
		CopyFromState cstate;

		Assert(rel);

		/* check read-only transaction and parallel mode */
		if (XactReadOnly && !rel->rd_islocaltemp)
			PreventCommandIfReadOnly("COPY FROM");

		cstate = BeginCopyFrom(pstate, rel, whereClause,
							   stmt->filename, stmt->is_program,
							   NULL, stmt->attlist, stmt->options);
		*processed = CopyFrom(cstate);	/* copy from file to database */
		EndCopyFrom(cstate);
	}
	else
	{
		CopyToState cstate;

		cstate = BeginCopyTo(pstate, rel, query, relid,
							 stmt->filename, stmt->is_program,
							 NULL, stmt->attlist, stmt->options);
		*processed = DoCopyTo(cstate);	/* copy from database to file */
		EndCopyTo(cstate);
	}

	if (rel != NULL)
		table_close(rel, NoLock);
}

/*
 * Extract the CopyFormatOptions.header_line value from a DefElem.
 *
 * Parses the HEADER option for COPY, which can be a boolean, a non-negative
 * integer (number of lines to skip), or the special value "match".
 */
static int
defGetCopyHeaderOption(DefElem *def, bool is_from)
{
	/*
	 * If no parameter value given, assume "true" is meant.
	 */
	if (def->arg == NULL)
		return COPY_HEADER_TRUE;

	/*
	 * Allow 0, 1, "true", "false", "on", "off", a non-negative integer, or
	 * "match".
	 */
	switch (nodeTag(def->arg))
	{
		case T_Integer:
			{
				int			ival = intVal(def->arg);

				if (ival < 0)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("a negative integer value cannot be "
									"specified for %s", def->defname)));

				if (!is_from && ival > 1)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("cannot use multi-line header in COPY TO")));

				return ival;
			}
			break;
		default:
			{
				char	   *sval = defGetString(def);

				/*
				 * The set of strings accepted here should match up with the
				 * grammar's opt_boolean_or_string production.
				 */
				if (pg_strcasecmp(sval, "true") == 0)
					return COPY_HEADER_TRUE;
				if (pg_strcasecmp(sval, "false") == 0)
					return COPY_HEADER_FALSE;
				if (pg_strcasecmp(sval, "on") == 0)
					return COPY_HEADER_TRUE;
				if (pg_strcasecmp(sval, "off") == 0)
					return COPY_HEADER_FALSE;
				if (pg_strcasecmp(sval, "match") == 0)
				{
					if (!is_from)
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("cannot use \"%s\" with HEADER in COPY TO",
										sval)));
					return COPY_HEADER_MATCH;
				}
			}
			break;
	}
	ereport(ERROR,
			(errcode(ERRCODE_SYNTAX_ERROR),
			 errmsg("%s requires a Boolean value, a non-negative integer, "
					"or the string \"match\"",
					def->defname)));
	return COPY_HEADER_FALSE;	/* keep compiler quiet */
}

/*
 * Extract a CopyOnErrorChoice value from a DefElem.
 */
static CopyOnErrorChoice
defGetCopyOnErrorChoice(DefElem *def, ParseState *pstate, bool is_from)
{
	char	   *sval = defGetString(def);

	if (!is_from)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
		/*- translator: first %s is the name of a COPY option, e.g. ON_ERROR,
		 second %s is a COPY with direction, e.g. COPY TO */
				 errmsg("COPY %s cannot be used with %s", "ON_ERROR", "COPY TO"),
				 parser_errposition(pstate, def->location)));

	/*
	 * Allow "stop", or "ignore" values.
	 */
	if (pg_strcasecmp(sval, "stop") == 0)
		return COPY_ON_ERROR_STOP;
	if (pg_strcasecmp(sval, "ignore") == 0)
		return COPY_ON_ERROR_IGNORE;

	ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
	/*- translator: first %s is the name of a COPY option, e.g. ON_ERROR */
			 errmsg("COPY %s \"%s\" not recognized", "ON_ERROR", sval),
			 parser_errposition(pstate, def->location)));
	return COPY_ON_ERROR_STOP;	/* keep compiler quiet */
}

/*
 * Extract REJECT_LIMIT value from a DefElem.
 *
 * REJECT_LIMIT can be specified in two ways: as an int64 for the COPY command
 * option or as a single-quoted string for the foreign table option using
 * file_fdw. Therefore this function needs to handle both formats.
 */
static int64
defGetCopyRejectLimitOption(DefElem *def)
{
	int64		reject_limit;

	if (def->arg == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("%s requires a numeric value",
						def->defname)));
	else if (nodeTag(def->arg) == T_String)
		reject_limit = pg_strtoint64(strVal(def->arg));
	else
		reject_limit = defGetInt64(def);

	if (reject_limit <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("REJECT_LIMIT (%" PRId64 ") must be greater than zero",
						reject_limit)));

	return reject_limit;
}

/*
 * Extract a CopyLogVerbosityChoice value from a DefElem.
 */
static CopyLogVerbosityChoice
defGetCopyLogVerbosityChoice(DefElem *def, ParseState *pstate)
{
	char	   *sval;

	/*
	 * Allow "silent", "default", or "verbose" values.
	 */
	sval = defGetString(def);
	if (pg_strcasecmp(sval, "silent") == 0)
		return COPY_LOG_VERBOSITY_SILENT;
	if (pg_strcasecmp(sval, "default") == 0)
		return COPY_LOG_VERBOSITY_DEFAULT;
	if (pg_strcasecmp(sval, "verbose") == 0)
		return COPY_LOG_VERBOSITY_VERBOSE;

	ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
	/*- translator: first %s is the name of a COPY option, e.g. ON_ERROR */
			 errmsg("COPY %s \"%s\" not recognized", "LOG_VERBOSITY", sval),
			 parser_errposition(pstate, def->location)));
	return COPY_LOG_VERBOSITY_DEFAULT;	/* keep compiler quiet */
}

/*
 * Process the statement option list for COPY.
 *
 * Scan the options list (a list of DefElem) and transpose the information
 * into *opts_out, applying appropriate error checking.
 *
 * If 'opts_out' is not NULL, it is assumed to be filled with zeroes initially.
 *
 * This is exported so that external users of the COPY API can sanity-check
 * a list of options.  In that usage, 'opts_out' can be passed as NULL and
 * the collected data is just leaked until CurrentMemoryContext is reset.
 *
 * Note that additional checking, such as whether column names listed in FORCE
 * QUOTE actually exist, has to be applied later.  This just checks for
 * self-consistency of the options list.
 */
void
ProcessCopyOptions(ParseState *pstate,
				   CopyFormatOptions *opts_out,
				   bool is_from,
				   List *options)
{
	bool		format_specified = false;
	bool		freeze_specified = false;
	bool		header_specified = false;
	bool		on_error_specified = false;
	bool		log_verbosity_specified = false;
	bool		reject_limit_specified = false;
	ListCell   *option;

	/* Support external use for option sanity checking */
	if (opts_out == NULL)
		opts_out = (CopyFormatOptions *) palloc0(sizeof(CopyFormatOptions));

	opts_out->file_encoding = -1;

	/* Extract options from the statement node tree */
	foreach(option, options)
	{
		DefElem    *defel = lfirst_node(DefElem, option);

		if (strcmp(defel->defname, "format") == 0)
		{
			char	   *fmt = defGetString(defel);

			if (format_specified)
				errorConflictingDefElem(defel, pstate);
			format_specified = true;
			if (strcmp(fmt, "text") == 0)
				 /* default format */ ;
			else if (strcmp(fmt, "csv") == 0)
				opts_out->csv_mode = true;
			else if (strcmp(fmt, "binary") == 0)
				opts_out->binary = true;
			else
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("COPY format \"%s\" not recognized", fmt),
						 parser_errposition(pstate, defel->location)));
		}
		else if (strcmp(defel->defname, "freeze") == 0)
		{
			if (freeze_specified)
				errorConflictingDefElem(defel, pstate);
			freeze_specified = true;
			opts_out->freeze = defGetBoolean(defel);
		}
		else if (strcmp(defel->defname, "delimiter") == 0)
		{
			if (opts_out->delim)
				errorConflictingDefElem(defel, pstate);
			opts_out->delim = defGetString(defel);
		}
		else if (strcmp(defel->defname, "null") == 0)
		{
			if (opts_out->null_print)
				errorConflictingDefElem(defel, pstate);
			opts_out->null_print = defGetString(defel);
		}
		else if (strcmp(defel->defname, "default") == 0)
		{
			if (opts_out->default_print)
				errorConflictingDefElem(defel, pstate);
			opts_out->default_print = defGetString(defel);
		}
		else if (strcmp(defel->defname, "header") == 0)
		{
			if (header_specified)
				errorConflictingDefElem(defel, pstate);
			header_specified = true;
			opts_out->header_line = defGetCopyHeaderOption(defel, is_from);
		}
		else if (strcmp(defel->defname, "quote") == 0)
		{
			if (opts_out->quote)
				errorConflictingDefElem(defel, pstate);
			opts_out->quote = defGetString(defel);
		}
		else if (strcmp(defel->defname, "escape") == 0)
		{
			if (opts_out->escape)
				errorConflictingDefElem(defel, pstate);
			opts_out->escape = defGetString(defel);
		}
		else if (strcmp(defel->defname, "force_quote") == 0)
		{
			if (opts_out->force_quote || opts_out->force_quote_all)
				errorConflictingDefElem(defel, pstate);
			if (defel->arg && IsA(defel->arg, A_Star))
				opts_out->force_quote_all = true;
			else if (defel->arg && IsA(defel->arg, List))
				opts_out->force_quote = castNode(List, defel->arg);
			else
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("argument to option \"%s\" must be a list of column names",
								defel->defname),
						 parser_errposition(pstate, defel->location)));
		}
		else if (strcmp(defel->defname, "force_not_null") == 0)
		{
			if (opts_out->force_notnull || opts_out->force_notnull_all)
				errorConflictingDefElem(defel, pstate);
			if (defel->arg && IsA(defel->arg, A_Star))
				opts_out->force_notnull_all = true;
			else if (defel->arg && IsA(defel->arg, List))
				opts_out->force_notnull = castNode(List, defel->arg);
			else
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("argument to option \"%s\" must be a list of column names",
								defel->defname),
						 parser_errposition(pstate, defel->location)));
		}
		else if (strcmp(defel->defname, "force_null") == 0)
		{
			if (opts_out->force_null || opts_out->force_null_all)
				errorConflictingDefElem(defel, pstate);
			if (defel->arg && IsA(defel->arg, A_Star))
				opts_out->force_null_all = true;
			else if (defel->arg && IsA(defel->arg, List))
				opts_out->force_null = castNode(List, defel->arg);
			else
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("argument to option \"%s\" must be a list of column names",
								defel->defname),
						 parser_errposition(pstate, defel->location)));
		}
		else if (strcmp(defel->defname, "convert_selectively") == 0)
		{
			/*
			 * Undocumented, not-accessible-from-SQL option: convert only the
			 * named columns to binary form, storing the rest as NULLs. It's
			 * allowed for the column list to be NIL.
			 */
			if (opts_out->convert_selectively)
				errorConflictingDefElem(defel, pstate);
			opts_out->convert_selectively = true;
			if (defel->arg == NULL || IsA(defel->arg, List))
				opts_out->convert_select = castNode(List, defel->arg);
			else
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("argument to option \"%s\" must be a list of column names",
								defel->defname),
						 parser_errposition(pstate, defel->location)));
		}
		else if (strcmp(defel->defname, "encoding") == 0)
		{
			if (opts_out->file_encoding >= 0)
				errorConflictingDefElem(defel, pstate);
			opts_out->file_encoding = pg_char_to_encoding(defGetString(defel));
			if (opts_out->file_encoding < 0)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("argument to option \"%s\" must be a valid encoding name",
								defel->defname),
						 parser_errposition(pstate, defel->location)));
		}
		else if (strcmp(defel->defname, "on_error") == 0)
		{
			if (on_error_specified)
				errorConflictingDefElem(defel, pstate);
			on_error_specified = true;
			opts_out->on_error = defGetCopyOnErrorChoice(defel, pstate, is_from);
		}
		else if (strcmp(defel->defname, "log_verbosity") == 0)
		{
			if (log_verbosity_specified)
				errorConflictingDefElem(defel, pstate);
			log_verbosity_specified = true;
			opts_out->log_verbosity = defGetCopyLogVerbosityChoice(defel, pstate);
		}
		else if (strcmp(defel->defname, "reject_limit") == 0)
		{
			if (reject_limit_specified)
				errorConflictingDefElem(defel, pstate);
			reject_limit_specified = true;
			opts_out->reject_limit = defGetCopyRejectLimitOption(defel);
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("option \"%s\" not recognized",
							defel->defname),
					 parser_errposition(pstate, defel->location)));
	}

	/*
	 * Check for incompatible options (must do these three before inserting
	 * defaults)
	 */
	if (opts_out->binary && opts_out->delim)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
		/*- translator: %s is the name of a COPY option, e.g. ON_ERROR */
				 errmsg("cannot specify %s in BINARY mode", "DELIMITER")));

	if (opts_out->binary && opts_out->null_print)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("cannot specify %s in BINARY mode", "NULL")));

	if (opts_out->binary && opts_out->default_print)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("cannot specify %s in BINARY mode", "DEFAULT")));

	/* Set defaults for omitted options */
	if (!opts_out->delim)
		opts_out->delim = opts_out->csv_mode ? "," : "\t";

	if (!opts_out->null_print)
		opts_out->null_print = opts_out->csv_mode ? "" : "\\N";
	opts_out->null_print_len = strlen(opts_out->null_print);

	if (opts_out->csv_mode)
	{
		if (!opts_out->quote)
			opts_out->quote = "\"";
		if (!opts_out->escape)
			opts_out->escape = opts_out->quote;
	}

	/* Only single-byte delimiter strings are supported. */
	if (strlen(opts_out->delim) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("COPY delimiter must be a single one-byte character")));

	/* Disallow end-of-line characters */
	if (strchr(opts_out->delim, '\r') != NULL ||
		strchr(opts_out->delim, '\n') != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("COPY delimiter cannot be newline or carriage return")));

	if (strchr(opts_out->null_print, '\r') != NULL ||
		strchr(opts_out->null_print, '\n') != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("COPY null representation cannot use newline or carriage return")));

	if (opts_out->default_print)
	{
		opts_out->default_print_len = strlen(opts_out->default_print);

		if (strchr(opts_out->default_print, '\r') != NULL ||
			strchr(opts_out->default_print, '\n') != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("COPY default representation cannot use newline or carriage return")));
	}

	/*
	 * Disallow unsafe delimiter characters in non-CSV mode.  We can't allow
	 * backslash because it would be ambiguous.  We can't allow the other
	 * cases because data characters matching the delimiter must be
	 * backslashed, and certain backslash combinations are interpreted
	 * non-literally by COPY IN.  Disallowing all lower case ASCII letters is
	 * more than strictly necessary, but seems best for consistency and
	 * future-proofing.  Likewise we disallow all digits though only octal
	 * digits are actually dangerous.
	 */
	if (!opts_out->csv_mode &&
		strchr("\\.abcdefghijklmnopqrstuvwxyz0123456789",
			   opts_out->delim[0]) != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("COPY delimiter cannot be \"%s\"", opts_out->delim)));

	/* Check header */
	if (opts_out->binary && opts_out->header_line != COPY_HEADER_FALSE)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		/*- translator: %s is the name of a COPY option, e.g. ON_ERROR */
				 errmsg("cannot specify %s in BINARY mode", "HEADER")));

	/* Check quote */
	if (!opts_out->csv_mode && opts_out->quote != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		/*- translator: %s is the name of a COPY option, e.g. ON_ERROR */
				 errmsg("COPY %s requires CSV mode", "QUOTE")));

	if (opts_out->csv_mode && strlen(opts_out->quote) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("COPY quote must be a single one-byte character")));

	if (opts_out->csv_mode && opts_out->delim[0] == opts_out->quote[0])
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("COPY delimiter and quote must be different")));

	/* Check escape */
	if (!opts_out->csv_mode && opts_out->escape != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		/*- translator: %s is the name of a COPY option, e.g. ON_ERROR */
				 errmsg("COPY %s requires CSV mode", "ESCAPE")));

	if (opts_out->csv_mode && strlen(opts_out->escape) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("COPY escape must be a single one-byte character")));

	/* Check force_quote */
	if (!opts_out->csv_mode && (opts_out->force_quote || opts_out->force_quote_all))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		/*- translator: %s is the name of a COPY option, e.g. ON_ERROR */
				 errmsg("COPY %s requires CSV mode", "FORCE_QUOTE")));
	if ((opts_out->force_quote || opts_out->force_quote_all) && is_from)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		/*- translator: first %s is the name of a COPY option, e.g. ON_ERROR,
		 second %s is a COPY with direction, e.g. COPY TO */
				 errmsg("COPY %s cannot be used with %s", "FORCE_QUOTE",
						"COPY FROM")));

	/* Check force_notnull */
	if (!opts_out->csv_mode && (opts_out->force_notnull != NIL ||
								opts_out->force_notnull_all))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		/*- translator: %s is the name of a COPY option, e.g. ON_ERROR */
				 errmsg("COPY %s requires CSV mode", "FORCE_NOT_NULL")));
	if ((opts_out->force_notnull != NIL || opts_out->force_notnull_all) &&
		!is_from)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
		/*- translator: first %s is the name of a COPY option, e.g. ON_ERROR,
		 second %s is a COPY with direction, e.g. COPY TO */
				 errmsg("COPY %s cannot be used with %s", "FORCE_NOT_NULL",
						"COPY TO")));

	/* Check force_null */
	if (!opts_out->csv_mode && (opts_out->force_null != NIL ||
								opts_out->force_null_all))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		/*- translator: %s is the name of a COPY option, e.g. ON_ERROR */
				 errmsg("COPY %s requires CSV mode", "FORCE_NULL")));

	if ((opts_out->force_null != NIL || opts_out->force_null_all) &&
		!is_from)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
		/*- translator: first %s is the name of a COPY option, e.g. ON_ERROR,
		 second %s is a COPY with direction, e.g. COPY TO */
				 errmsg("COPY %s cannot be used with %s", "FORCE_NULL",
						"COPY TO")));

	/* Don't allow the delimiter to appear in the null string. */
	if (strchr(opts_out->null_print, opts_out->delim[0]) != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
		/*- translator: %s is the name of a COPY option, e.g. NULL */
				 errmsg("COPY delimiter character must not appear in the %s specification",
						"NULL")));

	/* Don't allow the CSV quote char to appear in the null string. */
	if (opts_out->csv_mode &&
		strchr(opts_out->null_print, opts_out->quote[0]) != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
		/*- translator: %s is the name of a COPY option, e.g. NULL */
				 errmsg("CSV quote character must not appear in the %s specification",
						"NULL")));

	/* Check freeze */
	if (opts_out->freeze && !is_from)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
		/*- translator: first %s is the name of a COPY option, e.g. ON_ERROR,
		 second %s is a COPY with direction, e.g. COPY TO */
				 errmsg("COPY %s cannot be used with %s", "FREEZE",
						"COPY TO")));

	if (opts_out->default_print)
	{
		if (!is_from)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			/*- translator: first %s is the name of a COPY option, e.g. ON_ERROR,
			 second %s is a COPY with direction, e.g. COPY TO */
					 errmsg("COPY %s cannot be used with %s", "DEFAULT",
							"COPY TO")));

		/* Don't allow the delimiter to appear in the default string. */
		if (strchr(opts_out->default_print, opts_out->delim[0]) != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			/*- translator: %s is the name of a COPY option, e.g. NULL */
					 errmsg("COPY delimiter character must not appear in the %s specification",
							"DEFAULT")));

		/* Don't allow the CSV quote char to appear in the default string. */
		if (opts_out->csv_mode &&
			strchr(opts_out->default_print, opts_out->quote[0]) != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			/*- translator: %s is the name of a COPY option, e.g. NULL */
					 errmsg("CSV quote character must not appear in the %s specification",
							"DEFAULT")));

		/* Don't allow the NULL and DEFAULT string to be the same */
		if (opts_out->null_print_len == opts_out->default_print_len &&
			strncmp(opts_out->null_print, opts_out->default_print,
					opts_out->null_print_len) == 0)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("NULL specification and DEFAULT specification cannot be the same")));
	}
	/* Check on_error */
	if (opts_out->binary && opts_out->on_error != COPY_ON_ERROR_STOP)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("only ON_ERROR STOP is allowed in BINARY mode")));

	if (opts_out->reject_limit && !opts_out->on_error)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
		/*- translator: first and second %s are the names of COPY option, e.g.
		 * ON_ERROR, third is the value of the COPY option, e.g. IGNORE */
				 errmsg("COPY %s requires %s to be set to %s",
						"REJECT_LIMIT", "ON_ERROR", "IGNORE")));
}

/*
 * CopyGetAttnums - build an integer list of attnums to be copied
 *
 * The input attnamelist is either the user-specified column list,
 * or NIL if there was none (in which case we want all the non-dropped
 * columns).
 *
 * We don't include generated columns in the generated full list and we don't
 * allow them to be specified explicitly.  They don't make sense for COPY
 * FROM, but we could possibly allow them for COPY TO.  But this way it's at
 * least ensured that whatever we copy out can be copied back in.
 *
 * rel can be NULL ... it's only used for error reports.
 */
List *
CopyGetAttnums(TupleDesc tupDesc, Relation rel, List *attnamelist)
{
	List	   *attnums = NIL;

	if (attnamelist == NIL)
	{
		/* Generate default column list */
		int			attr_count = tupDesc->natts;
		int			i;

		for (i = 0; i < attr_count; i++)
		{
			CompactAttribute *attr = TupleDescCompactAttr(tupDesc, i);

			if (attr->attisdropped || attr->attgenerated)
				continue;
			attnums = lappend_int(attnums, i + 1);
		}
	}
	else
	{
		/* Validate the user-supplied list and extract attnums */
		ListCell   *l;

		foreach(l, attnamelist)
		{
			char	   *name = strVal(lfirst(l));
			int			attnum;
			int			i;

			/* Lookup column name */
			attnum = InvalidAttrNumber;
			for (i = 0; i < tupDesc->natts; i++)
			{
				Form_pg_attribute att = TupleDescAttr(tupDesc, i);

				if (att->attisdropped)
					continue;
				if (namestrcmp(&(att->attname), name) == 0)
				{
					if (att->attgenerated)
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
								 errmsg("column \"%s\" is a generated column",
										name),
								 errdetail("Generated columns cannot be used in COPY.")));
					attnum = att->attnum;
					break;
				}
			}
			if (attnum == InvalidAttrNumber)
			{
				if (rel != NULL)
					ereport(ERROR,
							(errcode(ERRCODE_UNDEFINED_COLUMN),
							 errmsg("column \"%s\" of relation \"%s\" does not exist",
									name, RelationGetRelationName(rel))));
				else
					ereport(ERROR,
							(errcode(ERRCODE_UNDEFINED_COLUMN),
							 errmsg("column \"%s\" does not exist",
									name)));
			}
			/* Check for duplicates */
			if (list_member_int(attnums, attnum))
				ereport(ERROR,
						(errcode(ERRCODE_DUPLICATE_COLUMN),
						 errmsg("column \"%s\" specified more than once",
								name)));
			attnums = lappend_int(attnums, attnum);
		}
	}

	return attnums;
}
