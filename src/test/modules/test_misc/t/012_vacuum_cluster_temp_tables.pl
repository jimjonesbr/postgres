# Copyright (c) 2026, PostgreSQL Global Development Group
#
# Verify that no-argument VACUUM FULL, CLUSTER, and REPACK skip temporary
# tables belonging to other sessions.
#
# A background session creates a temp table and marks its index as clustered —
# making it visible to both the pg_class scan (VACUUM FULL, REPACK) and the
# pg_index scan (CLUSTER) — then holds ACCESS SHARE LOCK in an open transaction.
# Each command runs with lock_timeout = '1ms'. Since lock_timeout only
# fires when a backend actually blocks waiting for a lock, 1ms is sufficient.

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('vacuum_cluster_temp');
$node->init;
$node->start;

# Session 1: build the temp table and hold a conflicting lock.
my $psql1 = $node->background_psql('postgres');

$psql1->query_safe(
	q{CREATE TEMP TABLE temp_repack_test (val int);
	  INSERT INTO temp_repack_test VALUES (1);
	  CREATE INDEX temp_repack_idx ON temp_repack_test (val);
	  CLUSTER temp_repack_test USING temp_repack_idx;});

$psql1->query_safe(q{BEGIN});
$psql1->query_safe(q{LOCK TABLE temp_repack_test IN ACCESS SHARE MODE});

my ($stdout, $stderr, $ret);

# VACUUM FULL — pg_class scan path.
$ret = $node->psql(
	'postgres',
	"SET lock_timeout = '1ms'; VACUUM FULL;",
	stdout => \$stdout,
	stderr => \$stderr);
is($ret, 0,
	'VACUUM FULL completes without blocking on another session temp table');

# CLUSTER — pg_index scan path (indisclustered entries).
$ret = $node->psql(
	'postgres',
	"SET lock_timeout = '1ms'; CLUSTER;",
	stdout => \$stdout,
	stderr => \$stderr);
is($ret, 0,
	'CLUSTER completes without blocking on another session temp table');

# REPACK — pg_class scan path.
$ret = $node->psql(
	'postgres',
	"SET lock_timeout = '1ms'; REPACK;",
	stdout => \$stdout,
	stderr => \$stderr);
is($ret, 0,
	'REPACK completes without blocking on another session temp table');

$psql1->quit;

done_testing();
