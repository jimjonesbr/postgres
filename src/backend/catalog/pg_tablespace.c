/*-------------------------------------------------------------------------
 *
 * pg_tablespace.c
 *	  routines to support tablespaces
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/catalog/pg_tablespace.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "access/amapi.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/table.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_am.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_language.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_partitioned_table.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_statistic_ext.h"
#include "catalog/pg_tablespace.h"
#include "catalog/pg_trigger.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "commands/tablespace.h"
#include "common/keywords.h"
#include "executor/spi.h"
#include "funcapi.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pathnodes.h"
#include "optimizer/optimizer.h"
#include "parser/parse_agg.h"
#include "parser/parse_func.h"
#include "parser/parse_oper.h"
#include "parser/parse_relation.h"
#include "parser/parser.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteHandler.h"
#include "rewrite/rewriteManip.h"
#include "rewrite/rewriteSupport.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/hsearch.h"
#include "utils/lsyscache.h"
#include "utils/partcache.h"
#include "utils/rel.h"
#include "utils/ruleutils.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/typcache.h"
#include "utils/varlena.h"
#include "utils/xml.h"

/*
 * get_tablespace_loc_string - Get a tablespace's location as a C-string.
 */
char *
get_tablespace_loc_string(Oid tablespaceOid)
{
	char		sourcepath[MAXPGPATH] = {'\0'};
	char		targetpath[MAXPGPATH] = {'\0'};
	int			rllen;
	struct stat st;
	StringInfoData buf;

	initStringInfo(&buf);
	appendStringInfoString(&buf, "");

	/*
	 * It's useful to apply this function to pg_class.reltablespace, wherein
	 * zero means "the database's default tablespace".  So, rather than
	 * throwing an error for zero, we choose to assume that's what is meant.
	 */
	if (tablespaceOid == InvalidOid)
		tablespaceOid = MyDatabaseTableSpace;

	/*
	 * Return empty string for the cluster's default tablespaces
	 */
	if (tablespaceOid == DEFAULTTABLESPACE_OID ||
		tablespaceOid == GLOBALTABLESPACE_OID)
		return buf.data;

	/*
	 * Find the location of the tablespace by reading the symbolic link that
	 * is in pg_tblspc/<oid>.
	 */
	snprintf(sourcepath, sizeof(sourcepath), "%s/%u", PG_TBLSPC_DIR, tablespaceOid);

	/*
	 * Before reading the link, check if the source path is a link or a
	 * junction point.  Note that a directory is possible for a tablespace
	 * created with allow_in_place_tablespaces enabled.  If a directory is
	 * found, a relative path to the data directory is returned.
	 */
	if (lstat(sourcepath, &st) < 0)
	{
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not stat file \"%s\": %m",
						sourcepath)));
	}

	if (!S_ISLNK(st.st_mode))
	{
		appendStringInfoString(&buf, sourcepath);
		return buf.data;
	}

	/*
	 * In presence of a link or a junction point, return the path pointing to.
	 */
	rllen = readlink(sourcepath, targetpath, sizeof(targetpath));
	if (rllen < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read symbolic link \"%s\": %m",
						sourcepath)));
	if (rllen >= sizeof(targetpath))
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("symbolic link \"%s\" target is too long",
						sourcepath)));
	targetpath[rllen] = '\0';

	appendStringInfoString(&buf, targetpath);

	return buf.data;
}
