/*-------------------------------------------------------------------------
 *
 * matview.c
 *	  materialized view support
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/matview.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/multixact.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_am.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_rewrite.h"
#include "commands/matview.h"
#include "commands/repack.h"
#include "commands/tablecmds.h"
#include "commands/tablespace.h"
#include "executor/executor.h"
#include "executor/spi.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "rewrite/rewriteHandler.h"
#include "rewrite/rewriteSupport.h"
#include "storage/lmgr.h"
#include "tcop/tcopprot.h"
#include "utils/acl.h"
#include "utils/hsearch.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"


typedef struct
{
	DestReceiver pub;			/* publicly-known function pointers */
	Oid			transientoid;	/* OID of new heap into which to store */
	/* These fields are filled by transientrel_startup: */
	Relation	transientrel;	/* relation to write to */
	CommandId	output_cid;		/* cmin to insert in output tuples */
	uint32		ti_options;		/* table_tuple_insert performance options */
	BulkInsertState bistate;	/* bulk insert state */
} DR_transientrel;

/*
 * State of a node during the depth-first walk in BuildViewDependencyGraph().
 * IN_PROGRESS marks a node on the current recursion path, which is how the
 * walk notices a cycle.
 */
typedef enum MatViewVisitState
{
	MATVIEW_UNVISITED,
	MATVIEW_IN_PROGRESS,
	MATVIEW_DONE,
} MatViewVisitState;

/*
 * One node of the view dependency graph used by REFRESH ALL MATERIALIZED
 * VIEWS.  There is a node for every view and materialized view the catalog
 * scan found; plain views are included because a matview can depend on
 * another matview through one.
 *
 * relkind and the names are captured during the unlocked catalog scan;
 * LockRelationsInOidOrder() is where they are revalidated.
 */
typedef struct MatViewRefreshNode
{
	Oid			oid;			/* hash key --- must be first field */
	char		relkind;		/* captured at discovery time */
	char	   *relname;
	char	   *nspname;
	bool		permitted;		/* may we refresh it?  matviews only */
	MatViewVisitState state;	/* dependency-walk state */
} MatViewRefreshNode;

/*
 * Working state for one REFRESH ALL MATERIALIZED VIEWS command.
 *
 * The nodes live in the hash table, which lets the dependency walk find a
 * relation's node by OID in constant time.  reloids lists the same relations
 * again so they can be iterated in a defined order; everything after
 * LockRelationsInOidOrder() relies on that order being ascending by OID.
 *
 * All of it is allocated in mcxt, so FreeMatViewRefreshContext() can release
 * the lot with one MemoryContextDelete().
 */
typedef struct MatViewRefreshContext
{
	MemoryContext mcxt;			/* everything below is allocated here */
	HTAB	   *nodes_by_oid;	/* Oid -> MatViewRefreshNode */
	List	   *reloids;		/* OIDs of every node; sorted into ascending */
	List	   *refresh_order;	/* matview OIDs, dependency order */
} MatViewRefreshContext;

static int	matview_maintenance_depth = 0;

static void transientrel_startup(DestReceiver *self, int operation, TupleDesc typeinfo);
static bool transientrel_receive(TupleTableSlot *slot, DestReceiver *self);
static void transientrel_shutdown(DestReceiver *self);
static void transientrel_destroy(DestReceiver *self);
static uint64 refresh_matview_datafill(DestReceiver *dest, Query *query,
									   const char *queryString, bool is_create);
static void refresh_by_match_merge(Oid matviewOid, Oid tempOid, Oid relowner,
								   int save_sec_context);
static void refresh_by_heap_swap(Oid matviewOid, Oid OIDNewHeap, char relpersistence);
static bool is_usable_unique_index(Relation indexRel);
static void OpenMatViewIncrementalMaintenance(void);
static void CloseMatViewIncrementalMaintenance(void);
static void InitMatViewRefreshContext(MatViewRefreshContext *context);
static void FreeMatViewRefreshContext(MatViewRefreshContext *context);
static MatViewRefreshNode *AddMatViewRefreshNode(MatViewRefreshContext *context, Form_pg_class classForm,
												 bool permitted);
static MatViewRefreshNode *FindMatViewRefreshNode(MatViewRefreshContext *context, Oid relid);
static List *MatViewGetDependencies(Oid relationOid);
static bool MatViewRefreshPermitted(Oid relOid);
static void BuildViewDependencyGraph(Oid relationOid, MatViewRefreshContext *context);
static void LockRelationsInOidOrder(MatViewRefreshContext *context, bool concurrent);
static bool MatViewCanRefreshConcurrently(Oid matviewOid);

/*
 * SetMatViewPopulatedState
 *		Mark a materialized view as populated, or not.
 *
 * NOTE: caller must be holding an appropriate lock on the relation.
 */
void
SetMatViewPopulatedState(Relation relation, bool newstate)
{
	Relation	pgrel;
	HeapTuple	tuple;

	Assert(relation->rd_rel->relkind == RELKIND_MATVIEW);

	/*
	 * Update relation's pg_class entry.  Crucial side-effect: other backends
	 * (and this one too!) are sent SI message to make them rebuild relcache
	 * entries.
	 */
	pgrel = table_open(RelationRelationId, RowExclusiveLock);
	tuple = SearchSysCacheCopy1(RELOID,
								ObjectIdGetDatum(RelationGetRelid(relation)));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u",
			 RelationGetRelid(relation));

	((Form_pg_class) GETSTRUCT(tuple))->relispopulated = newstate;

	CatalogTupleUpdate(pgrel, &tuple->t_self, tuple);

	heap_freetuple(tuple);
	table_close(pgrel, RowExclusiveLock);

	/*
	 * Advance command counter to make the updated pg_class row locally
	 * visible.
	 */
	CommandCounterIncrement();
}

/*
 * ExecRefreshMatView -- execute a REFRESH MATERIALIZED VIEW command
 *
 * If WITH NO DATA was specified, this is effectively like a TRUNCATE;
 * otherwise it is like a TRUNCATE followed by an INSERT using the SELECT
 * statement associated with the materialized view.  The statement node's
 * skipData field shows whether the clause was used.
 */
ObjectAddress
ExecRefreshMatView(RefreshMatViewStmt *stmt, const char *queryString,
				   QueryCompletion *qc)
{
	Oid			matviewOid;
	LOCKMODE	lockmode;

	/* Determine strength of lock needed. */
	lockmode = stmt->concurrent ? ExclusiveLock : AccessExclusiveLock;

	/*
	 * Get a lock until end of transaction.
	 */
	matviewOid = RangeVarGetRelidExtended(stmt->relation,
										  lockmode, 0,
										  RangeVarCallbackMaintainsTable,
										  NULL);

	return RefreshMatViewByOid(matviewOid, false, stmt->skipData,
							   stmt->concurrent, queryString, qc);
}

/*
 * InitMatViewRefreshContext
 *
 * Set up empty state for one REFRESH ALL MATERIALIZED VIEWS command.
 *
 * Everything the graph owns -- the hash table and its entries, the cached
 * relation names, and both Lists -- goes in a dedicated context so that
 * FreeMatViewRefreshContext() can release all of it at once.  We never reach
 * that call on the error path, but the context is a child of the caller's, so
 * abort processing cleans it up anyway.
 */
static void
InitMatViewRefreshContext(MatViewRefreshContext *context)
{
	HASHCTL ctl;

	context->mcxt = AllocSetContextCreate(CurrentMemoryContext,
										  "REFRESH ALL MATERIALIZED VIEWS",
										  ALLOCSET_DEFAULT_SIZES);

	context->reloids = NIL;
	context->refresh_order = NIL;

	ctl.keysize = sizeof(Oid);
	ctl.entrysize = sizeof(MatViewRefreshNode);
	ctl.hcxt = context->mcxt;

	context->nodes_by_oid = hash_create("REFRESH ALL MATERIALIZED VIEWS nodes",
										128,
										&ctl,
										HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
}

static void
FreeMatViewRefreshContext(MatViewRefreshContext *context)
{
	MemoryContextDelete(context->mcxt);

	context->mcxt = NULL;
	context->nodes_by_oid = NULL;
	context->reloids = NIL;
	context->refresh_order = NIL;
}

/*
 * AddMatViewRefreshNode
 *
 * Record one view or materialized view in the graph and return its node.
 *
 * "permitted" is meaningful only for materialized views; callers pass false
 * for plain views, which are recorded solely so the dependency walk can pass
 * through them.
 */
static MatViewRefreshNode *
AddMatViewRefreshNode(MatViewRefreshContext *context, Form_pg_class classForm,
					  bool permitted)
{
	MatViewRefreshNode *node;
	Oid relid = classForm->oid;
	MemoryContext oldcxt;
	bool found;

	node = (MatViewRefreshNode *)hash_search(context->nodes_by_oid, &relid,
											 HASH_ENTER, &found);
	Assert(!found); /* pg_class OIDs are unique */

	/* hash_search has already filled in node->oid */
	node->relkind = classForm->relkind;
	node->permitted = permitted;

	/*
	 * Take the names straight from the tuple we are looking at.  Going back
	 * to the syscache would use a fresh catalog snapshot, which can disagree
	 * with the snapshot of the scan that handed us this tuple and hand back a
	 * NULL relname for a concurrently dropped relation.
	 */
	node->state = MATVIEW_UNVISITED;

	oldcxt = MemoryContextSwitchTo(context->mcxt);

	/*
	 * Copy the names out of the tuple we were handed rather than looking them
	 * up again.  A syscache lookup would use a fresh catalog snapshot, which
	 * can disagree with the scan's snapshot and hand back NULL for a relation
	 * dropped in the meantime.
	 */
	node->relname = pstrdup(NameStr(classForm->relname));
	node->nspname = get_namespace_name(classForm->relnamespace);

	context->reloids = lappend_oid(context->reloids, relid);

	MemoryContextSwitchTo(oldcxt);

	return node;
}

/*
 * Look up the node for a relation, or NULL if the catalog scan never saw it.
 */
static MatViewRefreshNode *
FindMatViewRefreshNode(MatViewRefreshContext *context, Oid relid)
{
	return (MatViewRefreshNode *) hash_search(context->nodes_by_oid, &relid,
											  HASH_FIND, NULL);
}

/*
 * MatViewGetDependencies
 *
 * Return the OIDs of the views and materialized views that relationOid's
 * _RETURN rule reads from.
 *
 * Only direct dependencies are returned: if a matview selects from a plain
 * view that itself selects from another matview, only the plain view appears
 * here.  Walking the graph transitively is the caller's job.
 *
 * Other relation kinds are ignored, having no _RETURN rule and so no way to
 * lead to further matviews.  Dependencies that the rule does not record --
 * most notably a matview read by a function the view calls -- are invisible
 * here, and hence to the refresh ordering.
 *
 * The caller must hold a lock on relationOid; we rely on it to keep the rule
 * and its pg_depend entries from shifting under us.
 */
static List *
MatViewGetDependencies(Oid relationOid)
{
	HeapTuple	ruleTuple;
	Form_pg_rewrite ruleForm;
	Relation	dependDesc;
	SysScanDesc scan;
	ScanKeyData skey[2];
	HeapTuple	tuple;
	List	   *result = NIL;

	/*
	 * Find the relation's _RETURN rule.
	 */
	ruleTuple = SearchSysCache2(RULERELNAME,
								ObjectIdGetDatum(relationOid),
								PointerGetDatum(ViewSelectRuleName));

	if (!HeapTupleIsValid(ruleTuple))
		elog(ERROR, "could not find _RETURN rule for relation %u",
			 relationOid);

	ruleForm = (Form_pg_rewrite) GETSTRUCT(ruleTuple);

	/*
	 * Find the objects on which the _RETURN rule depends.
	 *
	 * The rule itself is the dependent object:
	 *
	 *     classid = RewriteRelationId
	 *     objid   = ruleForm->oid
	 *
	 * Dependencies recorded by rewriteDefine.c for objects referenced by
	 * the rule's action are NORMAL dependencies.  The INTERNAL dependency
	 * from the rule to its owning relation is deliberately ignored.
	 */
	dependDesc = table_open(DependRelationId, AccessShareLock);

	ScanKeyInit(&skey[0],
				Anum_pg_depend_classid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RewriteRelationId));

	ScanKeyInit(&skey[1],
				Anum_pg_depend_objid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(ruleForm->oid));

	scan = systable_beginscan(dependDesc,
							  DependDependerIndexId,
							  true,
							  NULL,
							  2,
							  skey);

	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		Form_pg_depend depend = (Form_pg_depend) GETSTRUCT(tuple);
		char		relkind;

		/*
		 * We are interested only in dependencies on relations.
		 */
		if (depend->refclassid != RelationRelationId ||
			depend->deptype != DEPENDENCY_NORMAL)
			continue;

		relkind = get_rel_relkind(depend->refobjid);

		/*
		 * Views and materialized views can themselves have _RETURN rules
		 * and therefore need to be traversed by the caller.  Other
		 * relation kinds terminate the dependency traversal.
		 */
		if (relkind != RELKIND_VIEW &&
			relkind != RELKIND_MATVIEW)
			continue;

		result = list_append_unique_oid(result, depend->refobjid);
	}

	systable_endscan(scan);
	table_close(dependDesc, AccessShareLock);

	ReleaseSysCache(ruleTuple);

	return result;
}

/*
 * MatViewRefreshPermitted
 *
 * Does the current user have the right to refresh this materialized view ---
 * that is, own it, or hold MAINTAIN on it?
 *
 * This duplicates the test RangeVarCallbackMaintainsTable() applies to the
 * single-view form, which raises an error rather than returning a verdict.
 * Keep the two in step.
 */
static bool
MatViewRefreshPermitted(Oid relOid)
{
	return object_ownercheck(RelationRelationId, relOid, GetUserId()) ||
		pg_class_aclcheck(relOid, GetUserId(), ACL_MAINTAIN) == ACLCHECK_OK;
}

/*
 * BuildViewDependencyGraph
 *
 * Walk the dependency graph rooted at relationOid depth-first, appending each
 * materialized view to context->refresh_order after everything it reads from.
 * Run from every root, this leaves refresh_order safe to refresh front to back.
 *
 * The walk does not filter on permission: unpermitted matviews are traversed
 * and appended like any other, since a permitted one may sit on the far side
 * of them.  Skipping those is the caller's job.
 *
 * The caller must have created a node for every relation reachable from
 * relationOid, and must hold a lock on each; we take none, and read the
 * _RETURN rules assuming they cannot change underneath us.  A relation with
 * no node is one the caller never saw and never locked, so rather than follow
 * the edge unprotected we error out and ask for the command to be retried.
 */
static void
BuildViewDependencyGraph(Oid relationOid, MatViewRefreshContext *context)
{
	MatViewRefreshNode *node;
	List	   *dependencies;
	ListCell   *lc;

	/* Guard against stack overflow due to deeply nested views */
	check_stack_depth();

	node = FindMatViewRefreshNode(context, relationOid);

	if (node == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("materialized view dependencies changed concurrently"),
				 errdetail("A view was redefined to depend on relation with OID %u, "
						   "which was not locked by REFRESH ALL MATERIALIZED VIEWS.",
						   relationOid),
				 errhint("Retry REFRESH ALL MATERIALIZED VIEWS.")));

	if (node->state == MATVIEW_DONE)
		return;

	if (node->state == MATVIEW_IN_PROGRESS)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("circular dependency detected in \"%s\"",
						quote_qualified_identifier(node->nspname, node->relname))));

	node->state = MATVIEW_IN_PROGRESS;
	dependencies = MatViewGetDependencies(relationOid);

	foreach(lc, dependencies)
		BuildViewDependencyGraph(lfirst_oid(lc), context);
	list_free(dependencies);

	node->state = MATVIEW_DONE;

	if (node->relkind == RELKIND_MATVIEW)
	{
		MemoryContext oldcxt = MemoryContextSwitchTo(context->mcxt);

		context->refresh_order = lappend_oid(context->refresh_order,
											 relationOid);
		MemoryContextSwitchTo(oldcxt);
	}
}

/*
 * LockRelationsInOidOrder
 *
 * Lock every relation in context->nodes, in ascending OID order, and re-check
 * under the lock what we recorded during the unlocked catalog scan.  Taking
 * the locks in a consistent order keeps this phase from deadlocking against
 * another session doing the same thing; it says nothing about the refresh
 * phase, which locks base relations in whatever order the plans require.
 *
 * Matviews we intend to refresh are locked with the mode the refresh itself
 * will use (AccessExclusiveLock, or ExclusiveLock when concurrent); every
 * other relation gets AccessShareLock, which is enough to block CREATE OR
 * REPLACE VIEW / DROP and thus freeze the _RETURN rules for the duration.
 *
 * Nodes for relations that were dropped between the scan and the lock are
 * removed from the list, so callers must be prepared for an OID recorded
 * during the scan to have no node afterwards.
 */
static void
LockRelationsInOidOrder(MatViewRefreshContext *context, bool concurrent)
{
	ListCell   *lc;

	/* Nothing depends on the discovery order, so sort in place. */
	list_sort(context->reloids, list_oid_cmp);

	foreach(lc, context->reloids)
	{
		Oid			relid = lfirst_oid(lc);
		MatViewRefreshNode *node = FindMatViewRefreshNode(context, relid);

		LOCKMODE	lockmode;

		Assert(node != NULL);

		/* Optimistic choice, based on the unlocked catalog scan. */
		if (node->relkind == RELKIND_MATVIEW && node->permitted)
			lockmode = concurrent ? ExclusiveLock : AccessExclusiveLock;
		else
			lockmode = AccessShareLock;

		LockRelationOid(relid, lockmode);

		/* Everything below is authoritative: the lock is now held. */

		/*
		 * The relation may have been dropped while we waited for the lock.
		 * Anything depending on it must have been dropped along with it, so
		 * we can simply forget about it rather than fail the whole command.
		 *
		 * get_rel_relkind() returns '\0' for a relation that no longer
		 * exists, and a live relation cannot change relkind in place, so a
		 * mismatch here means exactly one thing: it is gone.
		 */
		if (get_rel_relkind(relid) != node->relkind)
		{
			UnlockRelationOid(relid, lockmode);
			context->reloids = foreach_delete_current(context->reloids, lc);
			if (hash_search(context->nodes_by_oid, &relid,
							HASH_REMOVE, NULL) == NULL)
				elog(ERROR, "hash table corrupted");
			continue;
		}

		/*
		 * Re-check refresh permission now that we hold the lock.
		 *
		 * Only the revoke direction is actionable.  If permission was
		 * granted since the scan we hold only AccessShareLock, which is not
		 * strong enough to refresh under, so we leave the matview marked
		 * unpermitted and skip it: this command acts on the permissions in
		 * effect when it began.  If permission was revoked we already hold
		 * the stronger lock, so dropping the matview from consideration
		 * costs nothing.
		 *
		 * This is best-effort regardless, since GRANT and REVOKE do not lock
		 * the relation and so may change things again while we hold ours.
		 */
		if (node->relkind == RELKIND_MATVIEW && node->permitted)
			node->permitted = MatViewRefreshPermitted(relid);
	}
}

/*
 * MatViewCanRefreshConcurrently
 *
 * Can this materialized view be refreshed with CONCURRENTLY?  That needs it
 * to be populated, and to have a unique index.
 *
 * These are the same two conditions the single-view path enforces, where
 * failing either is an error; REFRESH ALL asks first so it can skip the
 * matview with a warning instead.  Keep the two in step.
 *
 * The caller holds a lock on the matview already, so we open with NoLock.
 */
static bool
MatViewCanRefreshConcurrently(Oid matviewOid)
{
	Relation	matviewRel;
	List	   *indexoidlist;
	ListCell   *indexoidscan;
	bool		usable = false;

	matviewRel = table_open(matviewOid, NoLock);

	/* Concurrent refresh requires a populated matview. */
	if (!RelationIsPopulated(matviewRel))
	{
		table_close(matviewRel, NoLock);
		return false;
	}

	/* ... and at least one usable unique index. */
	indexoidlist = RelationGetIndexList(matviewRel);
	foreach(indexoidscan, indexoidlist)
	{
		Oid			indexoid = lfirst_oid(indexoidscan);
		Relation	indexRel;

		indexRel = index_open(indexoid, AccessShareLock);
		usable = is_usable_unique_index(indexRel);
		index_close(indexRel, AccessShareLock);

		if (usable)
			break;
	}
	list_free(indexoidlist);

	table_close(matviewRel, NoLock);

	return usable;
}

/*
 * ExecRefreshAllMatViews -- implement REFRESH ALL MATERIALIZED VIEWS
 *
 * Refresh every materialized view in the current database that the current
 * user owns or holds MAINTAIN on, each one after everything it reads from,
 * directly or through plain views in between.  Matviews the user cannot
 * refresh are skipped rather than reported as errors, so the command succeeds
 * even when it refreshes nothing.
 *
 * Four steps:
 *  1) scan pg_class for every view and matview, plus the user's permission
 *     on each matview;
 *  2) lock the lot (LockRelationsInOidOrder);
 *  3) derive a refresh order from the now-frozen _RETURN rules
 *     (BuildViewDependencyGraph);
 *  4) refresh in that order. The first two steps are what let the last hand bare
 *     OIDs to RefreshMatViewByOid(), which neither locks nor checks permissions
 *     and expects its caller to have done both.
 *
 * Everything runs in the caller's transaction, so without CONCURRENTLY every
 * refreshed matview stays under AccessExclusiveLock until commit and they all
 * become visible at once.
 *
 * Returns InvalidObjectAddress: unlike the single-view form there is no one
 * object to report.  The command tag is set on qc before returning.
 */
ObjectAddress
ExecRefreshAllMatViews(RefreshMatViewStmt *stmt, const char *queryString,
					   QueryCompletion *qc)
{
	Relation	pgclassRel;
	TableScanDesc scan;
	HeapTuple	tuple;
	ListCell   *lc;
	MatViewRefreshContext ctx;
	int			npermitted = 0;

	/*
	 * CONCURRENTLY requires data, same as the single-statement form.
	 * This very same check is done in ExecRefreshMatView, but we do it
	 * here too to avoid doing the catalog scan and locking if the
	 * command is inevitably going to fail.
	 */
	if (stmt->concurrent && stmt->skipData)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("%s options %s and %s cannot be used together",
						"REFRESH", "CONCURRENTLY", "WITH NO DATA")));

	InitMatViewRefreshContext(&ctx);

	/*
	 * One catalog scan.  Every matview is a refresh root; we also record
	 * every view so the whole reachable graph can be locked below.  Locking
	 * all views (AccessShareLock) freezes their _RETURN rules, so a single
	 * discovery pass under the locks is authoritative -- no second pass and
	 * no concurrent-change comparison needed.
	 *
	 * Note: this takes AccessShareLock on every view in the database.  That
	 * is cheap in the common case but is a real lock-table cost in databases
	 * with very large numbers of views.
	 */
	pgclassRel = table_open(RelationRelationId, AccessShareLock);
	scan = table_beginscan_catalog(pgclassRel, 0, NULL);

	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Form_pg_class classForm = (Form_pg_class) GETSTRUCT(tuple);
		MatViewRefreshNode *node;

		if (classForm->relkind != RELKIND_MATVIEW &&
			classForm->relkind != RELKIND_VIEW)
			continue;

		/* Skip temp relations of other backends; we cannot access them */
		if (classForm->relpersistence == RELPERSISTENCE_TEMP &&
			!isTempNamespace(classForm->relnamespace))
			continue;

		node = AddMatViewRefreshNode(&ctx, classForm,
									 classForm->relkind == RELKIND_MATVIEW &&
									 MatViewRefreshPermitted(classForm->oid));

		if (classForm->relkind == RELKIND_MATVIEW)
		{
			if (!node->permitted)
				ereport(WARNING,
						(errmsg("permission denied to refresh \"%s\", skipping it",
								quote_qualified_identifier(node->nspname, node->relname))));
			else if (node->permitted)
				npermitted++;
		}
	}

	table_endscan(scan);
	table_close(pgclassRel, AccessShareLock);

	/* no permitted matviews to refresh still reports the right tag */
	if (npermitted == 0)
	{
		FreeMatViewRefreshContext(&ctx);

		if (qc)
			SetQueryCompletion(qc, CMDTAG_REFRESH_ALL_MATERIALIZED_VIEWS, 0);

		return InvalidObjectAddress;
	}

	/* Lock everything in a deadlock-safe order, rechecking under the lock. */
	LockRelationsInOidOrder(&ctx, stmt->concurrent);

	/*
	 * Walk the graph now that everything is locked.  Only permitted matviews
	 * are used as roots; the walk itself descends through whatever it finds.
	 * The nodes already exist, so this only sets visit states and fills
	 * refresh_order.
	 */
	foreach (lc, ctx.reloids)
	{
		Oid relid = lfirst_oid(lc);
		MatViewRefreshNode *node = FindMatViewRefreshNode(&ctx, relid);

		Assert(node != NULL);

		if (node->relkind == RELKIND_MATVIEW && node->permitted)
			BuildViewDependencyGraph(relid, &ctx);
	}

	/* Refresh in dependency order. */
	foreach(lc, ctx.refresh_order)
	{
		Oid			mv = lfirst_oid(lc);
		MatViewRefreshNode *node = FindMatViewRefreshNode(&ctx, mv);

		Assert(node != NULL);

		/*
		 * refresh_order only ever contains matviews reached from a permitted
		 * root, but a matview can be reached as a *dependency* of a permitted
		 * root while itself not being permitted.  Skip those.
		 */
		if (!node->permitted)
			continue;

		if (stmt->concurrent && !MatViewCanRefreshConcurrently(mv))
		{
			ereport(WARNING,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("cannot refresh materialized view \"%s\" concurrently, skipping it",
							quote_qualified_identifier(node->nspname, node->relname)),
					 errdetail("Concurrent refresh requires a populated materialized view with a unique index.")));
			continue;
		}

		if (stmt->verbose)
			ereport(INFO,
					(errmsg("refreshing materialized view \"%s\"",
							quote_qualified_identifier(node->nspname, node->relname))));

		RefreshMatViewByOid(mv, false, stmt->skipData,
							stmt->concurrent, queryString, NULL);
	}

	FreeMatViewRefreshContext(&ctx);

	if (qc)
		SetQueryCompletion(qc, CMDTAG_REFRESH_ALL_MATERIALIZED_VIEWS, 0);

	return InvalidObjectAddress;
}

/*
 * RefreshMatViewByOid -- refresh materialized view by OID
 *
 * This refreshes the materialized view by creating a new table and swapping
 * the relfilenumbers of the new table and the old materialized view, so the OID
 * of the original materialized view is preserved. Thus we do not lose GRANT
 * nor references to this materialized view.
 *
 * If skipData is true, this is effectively like a TRUNCATE; otherwise it is
 * like a TRUNCATE followed by an INSERT using the SELECT statement associated
 * with the materialized view.
 *
 * Indexes are rebuilt too, via REINDEX. Since we are effectively bulk-loading
 * the new heap, it's better to create the indexes afterwards than to fill them
 * incrementally while we load.
 *
 * The matview's "populated" state is changed based on whether the contents
 * reflect the result set of the materialized view's query.
 *
 * This is also used to populate the materialized view created by CREATE
 * MATERIALIZED VIEW command.
 */
ObjectAddress
RefreshMatViewByOid(Oid matviewOid, bool is_create, bool skipData,
					bool concurrent, const char *queryString,
					QueryCompletion *qc)
{
	Relation	matviewRel;
	RewriteRule *rule;
	List	   *actions;
	Query	   *dataQuery;
	Oid			tableSpace;
	Oid			relowner;
	Oid			OIDNewHeap;
	uint64		processed = 0;
	char		relpersistence;
	Oid			save_userid;
	int			save_sec_context;
	int			save_nestlevel;
	ObjectAddress address;

	matviewRel = table_open(matviewOid, NoLock);
	relowner = matviewRel->rd_rel->relowner;

	/*
	 * Switch to the owner's userid, so that any functions are run as that
	 * user.  Also lock down security-restricted operations and arrange to
	 * make GUC variable changes local to this command.
	 */
	GetUserIdAndSecContext(&save_userid, &save_sec_context);
	SetUserIdAndSecContext(relowner,
						   save_sec_context | SECURITY_RESTRICTED_OPERATION);
	save_nestlevel = NewGUCNestLevel();
	RestrictSearchPath();

	/* Make sure it is a materialized view. */
	if (matviewRel->rd_rel->relkind != RELKIND_MATVIEW)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("\"%s\" is not a materialized view",
						RelationGetRelationName(matviewRel))));

	/* Check that CONCURRENTLY is not specified if not populated. */
	if (concurrent && !RelationIsPopulated(matviewRel))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("CONCURRENTLY cannot be used when the materialized view is not populated")));

	/* Check that conflicting options have not been specified. */
	if (concurrent && skipData)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("%s options %s and %s cannot be used together",
						"REFRESH", "CONCURRENTLY", "WITH NO DATA")));

	/*
	 * Check that everything is correct for a refresh. Problems at this point
	 * are internal errors, so elog is sufficient.
	 */
	if (matviewRel->rd_rel->relhasrules == false ||
		matviewRel->rd_rules->numLocks < 1)
		elog(ERROR,
			 "materialized view \"%s\" is missing rewrite information",
			 RelationGetRelationName(matviewRel));

	if (matviewRel->rd_rules->numLocks > 1)
		elog(ERROR,
			 "materialized view \"%s\" has too many rules",
			 RelationGetRelationName(matviewRel));

	rule = matviewRel->rd_rules->rules[0];
	if (rule->event != CMD_SELECT || !(rule->isInstead))
		elog(ERROR,
			 "the rule for materialized view \"%s\" is not a SELECT INSTEAD OF rule",
			 RelationGetRelationName(matviewRel));

	actions = rule->actions;
	if (list_length(actions) != 1)
		elog(ERROR,
			 "the rule for materialized view \"%s\" is not a single action",
			 RelationGetRelationName(matviewRel));

	/*
	 * Check that there is a unique index with no WHERE clause on one or more
	 * columns of the materialized view if CONCURRENTLY is specified.
	 */
	if (concurrent)
	{
		List	   *indexoidlist = RelationGetIndexList(matviewRel);
		ListCell   *indexoidscan;
		bool		hasUniqueIndex = false;

		Assert(!is_create);

		foreach(indexoidscan, indexoidlist)
		{
			Oid			indexoid = lfirst_oid(indexoidscan);
			Relation	indexRel;

			indexRel = index_open(indexoid, AccessShareLock);
			hasUniqueIndex = is_usable_unique_index(indexRel);
			index_close(indexRel, AccessShareLock);
			if (hasUniqueIndex)
				break;
		}

		list_free(indexoidlist);

		if (!hasUniqueIndex)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("cannot refresh materialized view \"%s\" concurrently",
							quote_qualified_identifier(get_namespace_name(RelationGetNamespace(matviewRel)),
													   RelationGetRelationName(matviewRel))),
					 errhint("Create a unique index with no WHERE clause on one or more columns of the materialized view.")));
	}

	/*
	 * The stored query was rewritten at the time of the MV definition, but
	 * has not been scribbled on by the planner.
	 */
	dataQuery = linitial_node(Query, actions);

	/*
	 * Check for active uses of the relation in the current transaction, such
	 * as open scans.
	 *
	 * NB: We count on this to protect us against problems with refreshing the
	 * data using TABLE_INSERT_FROZEN.
	 */
	CheckTableNotInUse(matviewRel,
					   is_create ? "CREATE MATERIALIZED VIEW" :
					   "REFRESH MATERIALIZED VIEW");

	/*
	 * Tentatively mark the matview as populated or not, if its state is
	 * changing (this will roll back if we fail later).
	 */
	if (RelationIsPopulated(matviewRel) != !skipData)
		SetMatViewPopulatedState(matviewRel, !skipData);

	/* Concurrent refresh builds new data in temp tablespace, and does diff. */
	if (concurrent)
	{
		tableSpace = GetDefaultTablespace(RELPERSISTENCE_TEMP, false);
		relpersistence = RELPERSISTENCE_TEMP;
	}
	else
	{
		tableSpace = matviewRel->rd_rel->reltablespace;
		relpersistence = matviewRel->rd_rel->relpersistence;
	}

	/*
	 * Create the transient table that will receive the regenerated data. Lock
	 * it against access by any other process until commit (by which time it
	 * will be gone).
	 */
	OIDNewHeap = make_new_heap(matviewOid, tableSpace,
							   matviewRel->rd_rel->relam,
							   relpersistence, ExclusiveLock);
	Assert(CheckRelationOidLockedByMe(OIDNewHeap, AccessExclusiveLock, false));

	/* Generate the data, if wanted. */
	if (!skipData)
	{
		DestReceiver *dest;

		dest = CreateTransientRelDestReceiver(OIDNewHeap);
		processed = refresh_matview_datafill(dest, dataQuery, queryString,
											 is_create);
	}

	/* Make the matview match the newly generated data. */
	if (concurrent)
	{
		int			old_depth = matview_maintenance_depth;

		PG_TRY();
		{
			refresh_by_match_merge(matviewOid, OIDNewHeap, relowner,
								   save_sec_context);
		}
		PG_CATCH();
		{
			matview_maintenance_depth = old_depth;
			PG_RE_THROW();
		}
		PG_END_TRY();
		Assert(matview_maintenance_depth == old_depth);
	}
	else
	{
		refresh_by_heap_swap(matviewOid, OIDNewHeap, relpersistence);

		/*
		 * Inform cumulative stats system about our activity: basically, we
		 * truncated the matview and inserted some new data.  (The concurrent
		 * code path above doesn't need to worry about this because the
		 * inserts and deletes it issues get counted by lower-level code.)
		 */
		pgstat_count_truncate(matviewRel);
		if (!skipData)
			pgstat_count_heap_insert(matviewRel, processed);
	}

	table_close(matviewRel, NoLock);

	/* Roll back any GUC changes */
	AtEOXact_GUC(false, save_nestlevel);

	/* Restore userid and security context */
	SetUserIdAndSecContext(save_userid, save_sec_context);

	ObjectAddressSet(address, RelationRelationId, matviewOid);

	/*
	 * Save the rowcount so that pg_stat_statements can track the total number
	 * of rows processed by REFRESH MATERIALIZED VIEW command. Note that we
	 * still don't display the rowcount in the command completion tag output,
	 * i.e., the display_rowcount flag of CMDTAG_REFRESH_MATERIALIZED_VIEW
	 * command tag is left false in cmdtaglist.h. Otherwise, the change of
	 * completion tag output might break applications using it.
	 *
	 * When called from CREATE MATERIALIZED VIEW command, the rowcount is
	 * displayed with the command tag CMDTAG_SELECT.
	 */
	if (qc)
		SetQueryCompletion(qc,
						   is_create ? CMDTAG_SELECT : CMDTAG_REFRESH_MATERIALIZED_VIEW,
						   processed);

	return address;
}

/*
 * refresh_matview_datafill
 *
 * Execute the given query, sending result rows to "dest" (which will
 * insert them into the target matview).
 *
 * Returns number of rows inserted.
 */
static uint64
refresh_matview_datafill(DestReceiver *dest, Query *query,
						 const char *queryString, bool is_create)
{
	List	   *rewritten;
	PlannedStmt *plan;
	QueryDesc  *queryDesc;
	Query	   *copied_query;
	uint64		processed;

	/* Lock and rewrite, using a copy to preserve the original query. */
	copied_query = copyObject(query);
	AcquireRewriteLocks(copied_query, true, false);
	rewritten = QueryRewrite(copied_query);

	/* SELECT should never rewrite to more or less than one SELECT query */
	if (list_length(rewritten) != 1)
		elog(ERROR, "unexpected rewrite result for %s",
			 is_create ? "CREATE MATERIALIZED VIEW " : "REFRESH MATERIALIZED VIEW");
	query = (Query *) linitial(rewritten);

	/* Check for user-requested abort. */
	CHECK_FOR_INTERRUPTS();

	/* Plan the query which will generate data for the refresh. */
	plan = pg_plan_query(query, queryString, CURSOR_OPT_PARALLEL_OK, NULL, NULL);

	/*
	 * Use a snapshot with an updated command ID to ensure this query sees
	 * results of any previously executed queries.  (This could only matter if
	 * the planner executed an allegedly-stable function that changed the
	 * database contents, but let's do it anyway to be safe.)
	 */
	PushCopiedSnapshot(GetActiveSnapshot());
	UpdateActiveSnapshotCommandId();

	/* Create a QueryDesc, redirecting output to our tuple receiver */
	queryDesc = CreateQueryDesc(plan, queryString,
								GetActiveSnapshot(), InvalidSnapshot,
								dest, NULL, NULL, 0);

	/* call ExecutorStart to prepare the plan for execution */
	ExecutorStart(queryDesc, 0);

	/* run the plan */
	ExecutorRun(queryDesc, ForwardScanDirection, 0);

	processed = queryDesc->estate->es_processed;

	/* and clean up */
	ExecutorFinish(queryDesc);
	ExecutorEnd(queryDesc);

	FreeQueryDesc(queryDesc);

	PopActiveSnapshot();

	return processed;
}

DestReceiver *
CreateTransientRelDestReceiver(Oid transientoid)
{
	DR_transientrel *self = palloc0_object(DR_transientrel);

	self->pub.receiveSlot = transientrel_receive;
	self->pub.rStartup = transientrel_startup;
	self->pub.rShutdown = transientrel_shutdown;
	self->pub.rDestroy = transientrel_destroy;
	self->pub.mydest = DestTransientRel;
	self->transientoid = transientoid;

	return (DestReceiver *) self;
}

/*
 * transientrel_startup --- executor startup
 */
static void
transientrel_startup(DestReceiver *self, int operation, TupleDesc typeinfo)
{
	DR_transientrel *myState = (DR_transientrel *) self;
	Relation	transientrel;

	transientrel = table_open(myState->transientoid, NoLock);

	/*
	 * Fill private fields of myState for use by later routines
	 */
	myState->transientrel = transientrel;
	myState->output_cid = GetCurrentCommandId(true);
	myState->ti_options = TABLE_INSERT_SKIP_FSM | TABLE_INSERT_FROZEN;
	myState->bistate = GetBulkInsertState();

	/*
	 * Valid smgr_targblock implies something already wrote to the relation.
	 * This may be harmless, but this function hasn't planned for it.
	 */
	Assert(RelationGetTargetBlock(transientrel) == InvalidBlockNumber);
}

/*
 * transientrel_receive --- receive one tuple
 */
static bool
transientrel_receive(TupleTableSlot *slot, DestReceiver *self)
{
	DR_transientrel *myState = (DR_transientrel *) self;

	/*
	 * Note that the input slot might not be of the type of the target
	 * relation. That's supported by table_tuple_insert(), but slightly less
	 * efficient than inserting with the right slot - but the alternative
	 * would be to copy into a slot of the right type, which would not be
	 * cheap either. This also doesn't allow accessing per-AM data (say a
	 * tuple's xmin), but since we don't do that here...
	 */

	table_tuple_insert(myState->transientrel,
					   slot,
					   myState->output_cid,
					   myState->ti_options,
					   myState->bistate);

	/* We know this is a newly created relation, so there are no indexes */

	return true;
}

/*
 * transientrel_shutdown --- executor end
 */
static void
transientrel_shutdown(DestReceiver *self)
{
	DR_transientrel *myState = (DR_transientrel *) self;

	FreeBulkInsertState(myState->bistate);

	table_finish_bulk_insert(myState->transientrel, myState->ti_options);

	/* close transientrel, but keep lock until commit */
	table_close(myState->transientrel, NoLock);
	myState->transientrel = NULL;
}

/*
 * transientrel_destroy --- release DestReceiver object
 */
static void
transientrel_destroy(DestReceiver *self)
{
	pfree(self);
}

/*
 * refresh_by_match_merge
 *
 * Refresh a materialized view with transactional semantics, while allowing
 * concurrent reads.
 *
 * This is called after a new version of the data has been created in a
 * temporary table.  It performs a full outer join against the old version of
 * the data, producing "diff" results.  This join cannot work if there are any
 * duplicated rows in either the old or new versions, in the sense that every
 * column would compare as equal between the two rows.  It does work correctly
 * in the face of rows which have at least one NULL value, with all non-NULL
 * columns equal.  The behavior of NULLs on equality tests and on UNIQUE
 * indexes turns out to be quite convenient here; the tests we need to make
 * are consistent with default behavior.  If there is at least one UNIQUE
 * index on the materialized view, we have exactly the guarantee we need.
 *
 * The temporary table used to hold the diff results contains just the TID of
 * the old record (if matched) and the ROW from the new table as a single
 * column of complex record type (if matched).
 *
 * Once we have the diff table, we perform set-based DELETE and INSERT
 * operations against the materialized view, and discard both temporary
 * tables.
 *
 * Everything from the generation of the new data to applying the differences
 * takes place under cover of an ExclusiveLock, since it seems as though we
 * would want to prohibit not only concurrent REFRESH operations, but also
 * incremental maintenance.  It also doesn't seem reasonable or safe to allow
 * SELECT FOR UPDATE or SELECT FOR SHARE on rows being updated or deleted by
 * this command.
 */
static void
refresh_by_match_merge(Oid matviewOid, Oid tempOid, Oid relowner,
					   int save_sec_context)
{
	StringInfoData querybuf;
	Relation	matviewRel;
	Relation	tempRel;
	char	   *matviewname;
	char	   *tempname;
	char	   *diffname;
	char	   *temprelname;
	char	   *diffrelname;
	char	   *nsp;
	TupleDesc	tupdesc;
	bool		foundUniqueIndex;
	List	   *indexoidlist;
	ListCell   *indexoidscan;
	int16		relnatts;
	Oid		   *opUsedForQual;

	initStringInfo(&querybuf);
	matviewRel = table_open(matviewOid, NoLock);
	matviewname = quote_qualified_identifier(get_namespace_name(RelationGetNamespace(matviewRel)),
											 RelationGetRelationName(matviewRel));
	tempRel = table_open(tempOid, NoLock);

	/*
	 * Build qualified names of the temporary table and the diff table.  The
	 * only difference between them is the "_2" suffix on the diff table name.
	 */
	nsp = get_namespace_name(RelationGetNamespace(tempRel));
	temprelname = RelationGetRelationName(tempRel);
	diffrelname = psprintf("%s_2", temprelname);

	tempname = quote_qualified_identifier(nsp, temprelname);
	diffname = quote_qualified_identifier(nsp, diffrelname);

	relnatts = RelationGetNumberOfAttributes(matviewRel);

	/* Open SPI context. */
	SPI_connect();

	/* Analyze the temp table with the new contents. */
	appendStringInfo(&querybuf, "ANALYZE %s", tempname);
	if (SPI_exec(querybuf.data, 0) != SPI_OK_UTILITY)
		elog(ERROR, "SPI_exec failed: %s", querybuf.data);

	/*
	 * We need to ensure that there are not duplicate rows without NULLs in
	 * the new data set before we can count on the "diff" results.  Check for
	 * that in a way that allows showing the first duplicated row found.  Even
	 * after we pass this test, a unique index on the materialized view may
	 * find a duplicate key problem.
	 *
	 * Note: here and below, we use "tablename.*::tablerowtype" as a hack to
	 * keep ".*" from being expanded into multiple columns in a SELECT list.
	 * Compare ruleutils.c's get_variable().
	 */
	resetStringInfo(&querybuf);
	appendStringInfo(&querybuf,
					 "SELECT newdata.*::%s FROM %s newdata "
					 "WHERE newdata.* IS NOT NULL AND EXISTS "
					 "(SELECT 1 FROM %s newdata2 WHERE newdata2.* IS NOT NULL "
					 "AND newdata2.* OPERATOR(pg_catalog.*=) newdata.* "
					 "AND newdata2.ctid OPERATOR(pg_catalog.<>) "
					 "newdata.ctid)",
					 tempname, tempname, tempname);
	if (SPI_execute(querybuf.data, false, 1) != SPI_OK_SELECT)
		elog(ERROR, "SPI_exec failed: %s", querybuf.data);
	if (SPI_processed > 0)
	{
		/*
		 * Note that this ereport() is returning data to the user.  Generally,
		 * we would want to make sure that the user has been granted access to
		 * this data.  However, REFRESH MAT VIEW is only able to be run by the
		 * owner of the mat view (or a superuser) and therefore there is no
		 * need to check for access to data in the mat view.
		 */
		ereport(ERROR,
				(errcode(ERRCODE_CARDINALITY_VIOLATION),
				 errmsg("new data for materialized view \"%s\" contains duplicate rows without any null columns",
						RelationGetRelationName(matviewRel)),
				 errdetail("Row: %s",
						   SPI_getvalue(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1))));
	}

	/*
	 * Create the temporary "diff" table.
	 *
	 * Temporarily switch out of the SECURITY_RESTRICTED_OPERATION context,
	 * because you cannot create temp tables in SRO context.  For extra
	 * paranoia, add the composite type column only after switching back to
	 * SRO context.
	 */
	SetUserIdAndSecContext(relowner,
						   save_sec_context | SECURITY_LOCAL_USERID_CHANGE);
	resetStringInfo(&querybuf);
	appendStringInfo(&querybuf,
					 "CREATE TEMP TABLE %s (tid pg_catalog.tid)",
					 diffname);
	if (SPI_exec(querybuf.data, 0) != SPI_OK_UTILITY)
		elog(ERROR, "SPI_exec failed: %s", querybuf.data);
	SetUserIdAndSecContext(relowner,
						   save_sec_context | SECURITY_RESTRICTED_OPERATION);
	resetStringInfo(&querybuf);
	appendStringInfo(&querybuf,
					 "ALTER TABLE %s ADD COLUMN newdata %s",
					 diffname, tempname);
	if (SPI_exec(querybuf.data, 0) != SPI_OK_UTILITY)
		elog(ERROR, "SPI_exec failed: %s", querybuf.data);

	/* Start building the query for populating the diff table. */
	resetStringInfo(&querybuf);
	appendStringInfo(&querybuf,
					 "INSERT INTO %s "
					 "SELECT mv.ctid AS tid, newdata.*::%s AS newdata "
					 "FROM %s mv FULL JOIN %s newdata ON (",
					 diffname, tempname, matviewname, tempname);

	/*
	 * Get the list of index OIDs for the table from the relcache, and look up
	 * each one in the pg_index syscache.  We will test for equality on all
	 * columns present in all unique indexes which only reference columns and
	 * include all rows.
	 */
	tupdesc = matviewRel->rd_att;
	opUsedForQual = palloc0_array(Oid, relnatts);
	foundUniqueIndex = false;

	indexoidlist = RelationGetIndexList(matviewRel);

	foreach(indexoidscan, indexoidlist)
	{
		Oid			indexoid = lfirst_oid(indexoidscan);
		Relation	indexRel;

		indexRel = index_open(indexoid, RowExclusiveLock);
		if (is_usable_unique_index(indexRel))
		{
			Form_pg_index indexStruct = indexRel->rd_index;
			int			indnkeyatts = indexStruct->indnkeyatts;
			oidvector  *indclass;
			Datum		indclassDatum;
			int			i;

			/* Must get indclass the hard way. */
			indclassDatum = SysCacheGetAttrNotNull(INDEXRELID,
												   indexRel->rd_indextuple,
												   Anum_pg_index_indclass);
			indclass = (oidvector *) DatumGetPointer(indclassDatum);

			/* Add quals for all columns from this index. */
			for (i = 0; i < indnkeyatts; i++)
			{
				int			attnum = indexStruct->indkey.values[i];
				Oid			opclass = indclass->values[i];
				Form_pg_attribute attr = TupleDescAttr(tupdesc, attnum - 1);
				Oid			attrtype = attr->atttypid;
				HeapTuple	cla_ht;
				Form_pg_opclass cla_tup;
				Oid			opfamily;
				Oid			opcintype;
				Oid			op;
				const char *leftop;
				const char *rightop;

				/*
				 * Identify the equality operator associated with this index
				 * column.  First we need to look up the column's opclass.
				 */
				cla_ht = SearchSysCache1(CLAOID, ObjectIdGetDatum(opclass));
				if (!HeapTupleIsValid(cla_ht))
					elog(ERROR, "cache lookup failed for opclass %u", opclass);
				cla_tup = (Form_pg_opclass) GETSTRUCT(cla_ht);
				opfamily = cla_tup->opcfamily;
				opcintype = cla_tup->opcintype;
				ReleaseSysCache(cla_ht);

				op = get_opfamily_member_for_cmptype(opfamily, opcintype, opcintype, COMPARE_EQ);
				if (!OidIsValid(op))
					elog(ERROR, "missing equality operator for (%u,%u) in opfamily %u",
						 opcintype, opcintype, opfamily);

				/*
				 * If we find the same column with the same equality semantics
				 * in more than one index, we only need to emit the equality
				 * clause once.
				 *
				 * Since we only remember the last equality operator, this
				 * code could be fooled into emitting duplicate clauses given
				 * multiple indexes with several different opclasses ... but
				 * that's so unlikely it doesn't seem worth spending extra
				 * code to avoid.
				 */
				if (opUsedForQual[attnum - 1] == op)
					continue;
				opUsedForQual[attnum - 1] = op;

				/*
				 * Actually add the qual, ANDed with any others.
				 */
				if (foundUniqueIndex)
					appendStringInfoString(&querybuf, " AND ");

				leftop = quote_qualified_identifier("newdata",
													NameStr(attr->attname));
				rightop = quote_qualified_identifier("mv",
													 NameStr(attr->attname));

				generate_operator_clause(&querybuf,
										 leftop, attrtype,
										 op,
										 rightop, attrtype);

				foundUniqueIndex = true;
			}
		}

		/* Keep the locks, since we're about to run DML which needs them. */
		index_close(indexRel, NoLock);
	}

	list_free(indexoidlist);

	/*
	 * There must be at least one usable unique index on the matview.
	 *
	 * ExecRefreshMatView() checks that after taking the exclusive lock on the
	 * matview. So at least one unique index is guaranteed to exist here
	 * because the lock is still being held.  (One known exception is if a
	 * function called as part of refreshing the matview drops the index.
	 * That's a pretty silly thing to do.)
	 */
	if (!foundUniqueIndex)
		ereport(ERROR,
				errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("could not find suitable unique index on materialized view \"%s\"",
					   RelationGetRelationName(matviewRel)));

	appendStringInfoString(&querybuf,
						   " AND newdata.* OPERATOR(pg_catalog.*=) mv.*) "
						   "WHERE newdata.* IS NULL OR mv.* IS NULL "
						   "ORDER BY tid");

	/* Populate the temporary "diff" table. */
	if (SPI_exec(querybuf.data, 0) != SPI_OK_INSERT)
		elog(ERROR, "SPI_exec failed: %s", querybuf.data);

	/*
	 * We have no further use for data from the "full-data" temp table, but we
	 * must keep it around because its type is referenced from the diff table.
	 */

	/* Analyze the diff table. */
	resetStringInfo(&querybuf);
	appendStringInfo(&querybuf, "ANALYZE %s", diffname);
	if (SPI_exec(querybuf.data, 0) != SPI_OK_UTILITY)
		elog(ERROR, "SPI_exec failed: %s", querybuf.data);

	OpenMatViewIncrementalMaintenance();

	/* Deletes must come before inserts; do them first. */
	resetStringInfo(&querybuf);
	appendStringInfo(&querybuf,
					 "DELETE FROM %s mv WHERE ctid OPERATOR(pg_catalog.=) ANY "
					 "(SELECT diff.tid FROM %s diff "
					 "WHERE diff.tid IS NOT NULL "
					 "AND diff.newdata IS NULL)",
					 matviewname, diffname);
	if (SPI_exec(querybuf.data, 0) != SPI_OK_DELETE)
		elog(ERROR, "SPI_exec failed: %s", querybuf.data);

	/* Inserts go last. */
	resetStringInfo(&querybuf);
	appendStringInfo(&querybuf,
					 "INSERT INTO %s SELECT (diff.newdata).* "
					 "FROM %s diff WHERE tid IS NULL",
					 matviewname, diffname);
	if (SPI_exec(querybuf.data, 0) != SPI_OK_INSERT)
		elog(ERROR, "SPI_exec failed: %s", querybuf.data);

	/* We're done maintaining the materialized view. */
	CloseMatViewIncrementalMaintenance();
	table_close(tempRel, NoLock);
	table_close(matviewRel, NoLock);

	/* Clean up temp tables. */
	resetStringInfo(&querybuf);
	appendStringInfo(&querybuf, "DROP TABLE %s, %s", diffname, tempname);
	if (SPI_exec(querybuf.data, 0) != SPI_OK_UTILITY)
		elog(ERROR, "SPI_exec failed: %s", querybuf.data);

	/* Close SPI context. */
	if (SPI_finish() != SPI_OK_FINISH)
		elog(ERROR, "SPI_finish failed");
}

/*
 * Swap the physical files of the target and transient tables, then rebuild
 * the target's indexes and throw away the transient table.  Security context
 * swapping is handled by the called function, so it is not needed here.
 */
static void
refresh_by_heap_swap(Oid matviewOid, Oid OIDNewHeap, char relpersistence)
{
	finish_heap_swap(matviewOid, OIDNewHeap, false, false, true, true,
					 true,		/* reindex */
					 RecentXmin, ReadNextMultiXactId(), relpersistence);
}

/*
 * Check whether specified index is usable for match merge.
 */
static bool
is_usable_unique_index(Relation indexRel)
{
	Form_pg_index indexStruct = indexRel->rd_index;

	/*
	 * Must be unique, valid, immediate, non-partial, and be defined over
	 * plain user columns (not expressions).
	 */
	if (indexStruct->indisunique &&
		indexStruct->indimmediate &&
		indexStruct->indisvalid &&
		RelationGetIndexPredicate(indexRel) == NIL &&
		indexStruct->indnatts > 0)
	{
		/*
		 * The point of groveling through the index columns individually is to
		 * reject both index expressions and system columns.  Currently,
		 * matviews couldn't have OID columns so there's no way to create an
		 * index on a system column; but maybe someday that wouldn't be true,
		 * so let's be safe.
		 */
		int			numatts = indexStruct->indnatts;
		int			i;

		for (i = 0; i < numatts; i++)
		{
			int			attnum = indexStruct->indkey.values[i];

			if (attnum <= 0)
				return false;
		}
		return true;
	}
	return false;
}


/*
 * This should be used to test whether the backend is in a context where it is
 * OK to allow DML statements to modify materialized views.  We only want to
 * allow that for internal code driven by the materialized view definition,
 * not for arbitrary user-supplied code.
 *
 * While the function names reflect the fact that their main intended use is
 * incremental maintenance of materialized views (in response to changes to
 * the data in referenced relations), they are initially used to allow REFRESH
 * without blocking concurrent reads.
 */
bool
MatViewIncrementalMaintenanceIsEnabled(void)
{
	return matview_maintenance_depth > 0;
}

static void
OpenMatViewIncrementalMaintenance(void)
{
	matview_maintenance_depth++;
}

static void
CloseMatViewIncrementalMaintenance(void)
{
	matview_maintenance_depth--;
	Assert(matview_maintenance_depth >= 0);
}
