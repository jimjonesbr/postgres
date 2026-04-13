# Copyright (c) 2026, PostgreSQL Global Development Group

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use PostgreSQL::Test::BackgroundPsql;
use Test::More;

# Set up a fresh node
my $node = PostgreSQL::Test::Cluster->new('temp_lock');
$node->init;
$node->start;

# Create a long-lived session
my $psql1 = $node->background_psql('postgres');

$psql1->query_safe(q(CREATE TEMP TABLE foo AS SELECT 42 AS val;));

$psql1->query_safe(q(CREATE INDEX ON foo(val);));

my $tempschema = $node->safe_psql(
	'postgres',
	q{
      SELECT n.nspname
      FROM pg_class c
      JOIN pg_namespace n ON n.oid = c.relnamespace
      WHERE relname = 'foo' AND relpersistence = 't';
    }
);
chomp $tempschema;
ok($tempschema =~ /^pg_temp_\d+$/, "got temp schema: $tempschema");


# SELECT TEMPORARY TABLE from other session
my ($stdout, $stderr);
$node->psql(
	'postgres',
	"SELECT val FROM $tempschema.foo;",
	stdout => \$stdout,
	stderr => \$stderr);
like(
	$stderr,
	qr/cannot access temporary relations of other sessions/,
	'SELECT on other session temp table is not allowed');

# UPDATE TEMPORARY TABLE from other session
$node->psql(
	'postgres',
	"UPDATE $tempschema.foo SET val = NULL;",
	stderr => \$stderr);
like(
	$stderr,
	qr/cannot access temporary relations of other sessions/,
	'UPDATE on other session temp table is not allowed');

# DELETE records from TEMPORARY TABLE from other session
$node->psql('postgres', "DELETE FROM $tempschema.foo;", stderr => \$stderr);
like(
	$stderr,
	qr/cannot access temporary relations of other sessions/,
	'DELETE on other session temp table is not allowed');

# TRUNCATE TEMPORARY TABLE from other session
$node->psql('postgres', "TRUNCATE TABLE $tempschema.foo;",
	stderr => \$stderr);
like(
	$stderr,
	qr/cannot truncate temporary tables of other sessions/,
	'TRUNCATE on other session temp table is not allowed');

# INSERT INTO TEMPORARY TABLE from other session
$node->psql(
	'postgres',
	"INSERT INTO $tempschema.foo VALUES (73);",
	stderr => \$stderr);
like(
	$stderr,
	qr/cannot access temporary relations of other sessions/,
	'INSERT INTO on other session temp table is not allowed');

# COPY TEMPORARY TABLE from other session
$node->psql('postgres', "COPY $tempschema.foo TO STDOUT;",
	stderr => \$stderr);
like(
	$stderr,
	qr/cannot access temporary relations of other sessions/,
	'COPY on other session temp table is blocked');

# Index scans can use a different code path from the one sequential scans are
# following. Make sure that we cannot access other sessions' temp tables during
# index scan either.
$node->psql(
	'postgres',
	"SET enable_seqscan = off; SELECT val FROM $tempschema.foo WHERE val = 42;",
	stderr => \$stderr);
like(
	$stderr,
	qr/cannot access temporary relations of other sessions/,
	'index scan on other session temp table is not allowed (exercises ReadBufferExtended path)'
);

# DROP TEMPORARY TABLE from other session
$node->safe_psql('postgres', "DROP TABLE $tempschema.foo;");

# Clean up
$psql1->quit;

done_testing();
