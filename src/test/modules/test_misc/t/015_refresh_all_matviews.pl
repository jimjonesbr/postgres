# Copyright (c) 2026, PostgreSQL Global Development Group

# Tests for REFRESH ALL MATERIALIZED VIEWS.
#
# This command is database-wide, so it must run in an isolated database
# with a known set of objects; otherwise ordering, counts and VERBOSE
# output are non-deterministic.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->start;

my $db = 'mvtest';
$node->safe_psql('postgres', "CREATE DATABASE $db");

# ---------------------------------------------------------------------------
# 1. No matviews: succeeds as a no-op.
# ---------------------------------------------------------------------------
$node->safe_psql($db, 'REFRESH ALL MATERIALIZED VIEWS');
pass('REFRESH ALL with no materialized views is a no-op');

# ---------------------------------------------------------------------------
# 2. Dependency ordering across a matview chain.
#
# All created WITH NO DATA (unpopulated).  A dependent matview cannot be
# refreshed before the one it selects from, so this only succeeds if the
# refresh order is t1 -> mv1 -> mv2 -> mv3.
# ---------------------------------------------------------------------------
$node->safe_psql($db, qq{
    CREATE TABLE t1 AS SELECT 'foo'::text AS c;
    CREATE MATERIALIZED VIEW mv1 AS SELECT c FROM t1  WITH NO DATA;
    CREATE MATERIALIZED VIEW mv2 AS SELECT c FROM mv1 WITH NO DATA;
    CREATE MATERIALIZED VIEW mv3 AS SELECT c FROM mv2 WITH NO DATA;
});

$node->safe_psql($db, 'REFRESH ALL MATERIALIZED VIEWS');
is($node->safe_psql($db, 'SELECT c FROM mv3'), 'foo',
   'chained matviews refreshed in dependency order');

# ---------------------------------------------------------------------------
# 3. A single REFRESH ALL propagates base-table changes through the chain.
# ---------------------------------------------------------------------------
$node->safe_psql($db, "UPDATE t1 SET c = 'bar'");
$node->safe_psql($db, 'REFRESH ALL MATERIALIZED VIEWS');
is($node->safe_psql($db, 'SELECT c FROM mv3'), 'bar',
   'base-table change reaches leaf matview in one command');

# ---------------------------------------------------------------------------
# 4. Ordering is respected when a plain view sits between two matviews.
#    mvb depends on v, which depends on mva -> mva must refresh first.
# ---------------------------------------------------------------------------
$node->safe_psql($db, qq{
    CREATE MATERIALIZED VIEW mva AS SELECT c FROM t1 WITH NO DATA;
    CREATE VIEW v AS SELECT c FROM mva;
    CREATE MATERIALIZED VIEW mvb AS SELECT c FROM v  WITH NO DATA;
});
$node->safe_psql($db, 'REFRESH ALL MATERIALIZED VIEWS');
is($node->safe_psql($db, 'SELECT c FROM mvb'), 'bar',
   'ordering respected through an intermediate plain view');

# ---------------------------------------------------------------------------
# 5. VERBOSE reports every refresh.
# ---------------------------------------------------------------------------
my ($ret, $out, $err) =
  $node->psql($db, 'REFRESH ALL MATERIALIZED VIEWS VERBOSE');
is($ret, 0, 'REFRESH ALL VERBOSE succeeds');

for my $mv (qw(mv1 mv2 mv3 mva mvb))
{
    like($err, qr/refreshing materialized view "public\.$mv"/,
         "VERBOSE reports refresh of $mv");
}

# VERBOSE output is emitted in refresh order, so the dependency chain must
# appear in order.
ok(index($err, 'public.mv1') < index($err, 'public.mv2')
       && index($err, 'public.mv2') < index($err, 'public.mv3'),
   'VERBOSE lines appear in dependency order');

# ---------------------------------------------------------------------------
# 6. Permissions: an unprivileged role refreshes nothing, skips everything,
#    and does not error.
# ---------------------------------------------------------------------------
$node->safe_psql($db, 'CREATE ROLE u1 LOGIN');
($ret, $out, $err) = $node->psql(
    $db, 'REFRESH ALL MATERIALIZED VIEWS',
    extra_params => [ '--username' => 'u1' ]);
is($ret, 0, 'unprivileged REFRESH ALL succeeds as a no-op');
like($err, qr/permission denied to refresh "public\.mv3", skipping it/,
     'report WARNING for skipped matview with unprivileged role');

# ---------------------------------------------------------------------------
# 7. CONCURRENTLY refreshes eligible matviews and skips ineligible ones
#    (no unique index) with a warning, rather than aborting the batch.
# ---------------------------------------------------------------------------
$node->safe_psql($db, qq{
    CREATE MATERIALIZED VIEW mvc AS SELECT c FROM t1;
    CREATE UNIQUE INDEX ON mvc (c);
});

($ret, $out, $err) =
  $node->psql($db, 'REFRESH ALL MATERIALIZED VIEWS CONCURRENTLY');
is($ret, 0, 'REFRESH ALL CONCURRENTLY succeeds, skipping ineligible matviews');
like($err, qr/cannot refresh materialized view "public\.mv1" concurrently, skipping it/,
     'matview without unique index is skipped, not fatal');
is($node->safe_psql($db, 'SELECT c FROM mvc'), 'bar',
   'eligible matview refreshed under CONCURRENTLY');

# ---------------------------------------------------------------------------
# 8. CONCURRENTLY + WITH NO DATA is rejected.
# ---------------------------------------------------------------------------
($ret, $out, $err) =
  $node->psql($db, 'REFRESH ALL MATERIALIZED VIEWS CONCURRENTLY WITH NO DATA');
isnt($ret, 0, 'CONCURRENTLY WITH NO DATA is rejected');
like($err, qr/REFRESH options CONCURRENTLY and WITH NO DATA cannot be used together/,
     'correct error for CONCURRENTLY WITH NO DATA');

# ---------------------------------------------------------------------------
# 9. CONCURRENTLY: refreshing a matview whose dependency was skipped for being
#    unpopulated errors, matching single-view REFRESH semantics, and rolls back
#    everything the command had already refreshed.
# ---------------------------------------------------------------------------
$node->safe_psql($db, qq{
    -- Witness: eligible for concurrent refresh (populated + unique index) and
    -- created first, so it has the lowest OID of this block and REFRESH ALL
    -- refreshes it successfully before mvtop errors.
    CREATE MATERIALIZED VIEW mvwitness AS SELECT c FROM t1;
    CREATE UNIQUE INDEX ON mvwitness (c);

    -- mvtop is itself eligible (populated + unique index), but reads from
    -- mvdep, which we leave unpopulated so mvtop's concurrent refresh errors.
    CREATE MATERIALIZED VIEW mvdep AS SELECT c FROM t1;
    CREATE MATERIALIZED VIEW mvtop AS SELECT c FROM mvdep;
    CREATE UNIQUE INDEX ON mvtop (c);
    REFRESH MATERIALIZED VIEW mvdep WITH NO DATA;   -- now unpopulated
});

# Capture mvwitness's current contents, then dirty its base data so that a
# successful refresh *would* change it.  Capturing the value keeps the test
# independent of whatever earlier subtests left in t1.
my $before = $node->safe_psql($db, 'SELECT c FROM mvwitness');
$node->safe_psql($db, "UPDATE t1 SET c = c || '_changed'");

($ret, $out, $err) =
  $node->psql($db, 'REFRESH ALL MATERIALIZED VIEWS CONCURRENTLY');
isnt($ret, 0,
     'CONCURRENTLY errors when a dependent reads a skipped, unpopulated matview');
like($err, qr/has not been populated/,
     'error matches stand-alone REFRESH on an unpopulated matview');

# The command runs in one transaction, so the error must roll back the refresh
# it already performed on mvwitness: it must still show the pre-command value.
is($node->safe_psql($db, 'SELECT c FROM mvwitness'), $before,
   'failed REFRESH ALL CONCURRENTLY rolls back matviews already refreshed');

# Clean up so a later REFRESH ALL CONCURRENTLY isn't permanently poisoned by
# the unpopulated mvdep.
$node->safe_psql($db, 'DROP MATERIALIZED VIEW mvtop, mvdep, mvwitness');

# ---------------------------------------------------------------------------
# 10. A permitted matview depending on a non-permitted one is refreshed
#     against that dependency's current (stale) contents, without erroring
#     and without refreshing the dependency itself.
# ---------------------------------------------------------------------------
$node->safe_psql($db, 'CREATE ROLE u2 LOGIN');

$node->safe_psql($db, qq{
    CREATE TABLE t_stale (c text);
    INSERT INTO t_stale VALUES ('orig');
    CREATE MATERIALIZED VIEW mv_parent AS SELECT c FROM t_stale;    -- 'orig'
    CREATE MATERIALIZED VIEW mv_child  AS SELECT c FROM mv_parent;  -- 'orig'
    ALTER MATERIALIZED VIEW mv_child OWNER TO u2;
    GRANT SELECT ON mv_parent TO u2;

    UPDATE t_stale SET c = 'stale';
    REFRESH MATERIALIZED VIEW mv_parent;   -- mv_parent = 'stale'
    UPDATE t_stale SET c = 'current';      -- base moves on; mv_parent stays 'stale'
});

($ret, $out, $err) = $node->psql(
    $db, 'REFRESH ALL MATERIALIZED VIEWS',
    extra_params => [ '--username' => 'u2' ]);
is($ret, 0, 'REFRESH ALL succeeds for u2 despite a non-permitted dependency');

like($err, qr/permission denied to refresh "public\.mv_parent", skipping it/,
     'non-permitted dependency reported as skipped');
is($node->safe_psql($db, 'SELECT c FROM mv_child'), 'stale',
   q(dependent refreshed against skipped dependency's current (stale) contents));
is($node->safe_psql($db, 'SELECT c FROM mv_parent'), 'stale',
   'non-permitted dependency was left unrefreshed');

$node->safe_psql($db, 'DROP MATERIALIZED VIEW mv_child, mv_parent; DROP TABLE t_stale;');

# ---------------------------------------------------------------------------
# 11. REFRESH ALL fires event triggers under the correct command tag.
#     This also verifies CreateCommandTag() branches on the ALL form and that
#     the command tag's event-trigger flag is set.
# ---------------------------------------------------------------------------
$node->safe_psql($db, q{
    CREATE FUNCTION log_ddl() RETURNS event_trigger LANGUAGE plpgsql AS
      $$ BEGIN RAISE NOTICE 'evt=% tag=%', tg_event, tg_tag; END $$;
    CREATE EVENT TRIGGER refresh_all_start ON ddl_command_start
      WHEN TAG IN ('REFRESH ALL MATERIALIZED VIEWS') EXECUTE FUNCTION log_ddl();
    CREATE EVENT TRIGGER refresh_all_end ON ddl_command_end
      WHEN TAG IN ('REFRESH ALL MATERIALIZED VIEWS') EXECUTE FUNCTION log_ddl();
});

($ret, $out, $err) = $node->psql($db, 'REFRESH ALL MATERIALIZED VIEWS');
is($ret, 0, 'REFRESH ALL succeeds with event triggers installed');
like($err, qr/evt=ddl_command_start tag=REFRESH ALL MATERIALIZED VIEWS/,
     'ddl_command_start fires with the REFRESH ALL MATERIALIZED VIEWS tag');
like($err, qr/evt=ddl_command_end tag=REFRESH ALL MATERIALIZED VIEWS/,
     'ddl_command_end fires with the REFRESH ALL MATERIALIZED VIEWS tag');

$node->safe_psql($db,
    'DROP EVENT TRIGGER refresh_all_start;
     DROP EVENT TRIGGER refresh_all_end;
     DROP FUNCTION log_ddl();');

# ---------------------------------------------------------------------------
# 12. Temporary matviews: another session's are ignored, our own are refreshed.
# ---------------------------------------------------------------------------
my $other = $node->background_psql($db);
$other->query_safe(
	"CREATE MATERIALIZED VIEW pg_temp.other_session_mv AS SELECT 1 AS c");

($ret, $out, $err) = $node->psql($db, 'REFRESH ALL MATERIALIZED VIEWS VERBOSE');
is($ret, 0, "REFRESH ALL ignores another session's temporary matview");
unlike($err, qr/other_session_mv/,
	"another session's temporary matview is not touched");

$other->quit;

# The session's own temp matview must still be refreshed
is( $node->safe_psql(
		$db, q{
	CREATE MATERIALIZED VIEW pg_temp.own_mv AS SELECT 1 AS c WITH NO DATA;
	REFRESH ALL MATERIALIZED VIEWS;
	SELECT c FROM own_mv;
}),
	'1',
	"session's own temporary matview is refreshed");

$node->stop;
done_testing();
