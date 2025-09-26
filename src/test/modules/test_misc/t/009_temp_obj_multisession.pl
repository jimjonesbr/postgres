# Copyright (c) 2025, PostgreSQL Global Development Group

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

$psql1->query_safe(
	q(CREATE TEMP TABLE foo AS SELECT 42 AS val;));

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
    stderr => \$stderr
);
like($stderr, qr/cannot access temporary relations of other sessions/,
     'SELECT on other session temp table is not allowed');

# UPDATE TEMPORARY TABLE from other session
$node->psql(
    'postgres',
    "UPDATE $tempschema.foo SET val = NULL;",
    stderr => \$stderr
);
like($stderr, qr/cannot access temporary relations of other sessions/,
     'UPDATE on other session temp table is not allowed');

# DELETE records from TEMPORARY TABLE from other session
$node->psql(
    'postgres',
    "DELETE FROM $tempschema.foo;",
    stderr => \$stderr
);
like($stderr, qr/cannot access temporary relations of other sessions/,
     'DELETE on other session temp table is not allowed');

# TRUNCATE TEMPORARY TABLE from other session
$node->psql(
    'postgres',
    "TRUNCATE TABLE $tempschema.foo;",
    stderr => \$stderr
);
like($stderr, qr/cannot access temporary relations of other sessions/,
     'TRUNCATE on other session temp table is not allowed');

# INSERT INTO TEMPORARY TABLE from other session
$node->psql(
    'postgres',
    "INSERT INTO $tempschema.foo VALUES (73);",
    stderr => \$stderr
);
like($stderr, qr/cannot access temporary relations of other sessions/,
     'INSERT INTO on other session temp table is not allowed');

# ALTER TABLE .. RENAME TEMPORARY TABLE from other session
$node->psql(
    'postgres',
    "ALTER TABLE $tempschema.foo RENAME TO bar;",
    stderr => \$stderr
);
like($stderr, qr/cannot access temporary relations of other sessions/,
     'ALTER TABLE ... RENAME on other session temp table is blocked');

# ALTER TABLE .. ADD COLUMN in TEMPORARY TABLE from other session
$node->psql(
    'postgres',
    "ALTER TABLE $tempschema.foo ADD COLUMN bar int;",
    stderr => \$stderr
);
like($stderr, qr/cannot access temporary relations of other sessions/,
     'ALTER TABLE ... ADD COLUMN on other session temp table is blocked');

# COPY TEMPORARY TABLE from other session
$node->psql(
    'postgres',
    "COPY $tempschema.foo TO '/tmp/x';",
    stderr => \$stderr
);
like($stderr, qr/cannot access temporary relations of other sessions/,
     'COPY on other session temp table is blocked');

# LOCK TEMPORARY TABLE from other session
$node->psql(
    'postgres',
    "BEGIN; LOCK TABLE $tempschema.foo IN ACCESS EXCLUSIVE MODE;",
    stderr => \$stderr
);
like($stderr, qr/cannot access temporary relations of other sessions/,
     'LOCK on other session temp table is blocked');

# DROP TEMPORARY TABLE from other session
my $ok = $node->psql(
    'postgres',
    "DROP TABLE $tempschema.foo;"
);
ok($ok == 0, 'DROP TABLE executed successfully');

# Clean up
$psql1->quit;

done_testing();
