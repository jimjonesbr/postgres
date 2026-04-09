# Copyright (c) 2026, PostgreSQL Global Development Group

# Test log_statement_max_length GUC: verifies that logged statement text is
# truncated at the specified byte limit, respecting multibyte boundaries, for
# both log_statement and log_min_duration_statement logging.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('primary');
$node->init();
$node->start;

# Verify truncation works with ASCII.  The query is 24 bytes; with
# log_statement_max_length = 20 it must be cut after the 20th byte ('C')
# and must NOT contain the 21st character ('D').
note "ASCII truncation via log_statement";
my $log_offset = -s $node->logfile;
$node->psql('postgres', "
	SET log_statement_max_length TO 20;
	SELECT '123456789ABCDEF';");
ok( $node->log_contains(
		qr/statement: SELECT '123456789ABC(?!D)/, $log_offset),
	"ASCII query truncated at 20 bytes");

# Verify -1 logs statement in full (closing quote must be present).
note "-1 logs statement in full";
$log_offset = -s $node->logfile;
$node->psql('postgres', "
	SET log_statement_max_length TO -1;
	SELECT '123456789ABCDEF';");
ok( $node->log_contains(
		qr/statement: SELECT '123456789ABCDEF'/, $log_offset),
	"-1 logs full query");

# Verify multibyte character handling: truncation must not split a multibyte
# character (the 🐘 emoji is 4 bytes; with limit=12 it must be kept whole
# and the following 't' must not appear).
note "Multibyte truncation respects character boundaries";
$log_offset = -s $node->logfile;
$node->psql('postgres', "
	SET log_statement_max_length TO 12;
	SELECT '\xF0\x9F\x90\x98test';");
ok( $node->log_contains(
		qr/SELECT '\xF0\x9F\x90\x98(?!t)/, $log_offset),
	"multibyte truncation at character boundary");

# Verify truncation via the extended query protocol (execute message).
# Same 24-byte query truncated to 20 bytes; the 21st character ('D') must
# not appear.
note "Extended query protocol (execute) truncation";
$log_offset = -s $node->logfile;
$node->psql('postgres', "
	SET log_statement_max_length TO 20;
	SELECT '123456789ABCDEF' \\bind \\g");
ok( $node->log_contains(
		qr/execute <unnamed>: SELECT '123456789ABC(?!D)/, $log_offset),
	"extended protocol execute truncated at 20 bytes");

# Verify extended protocol also respects -1 (no truncation; closing quote
# present).
note "Extended query protocol with -1 (no truncation)";
$log_offset = -s $node->logfile;
$node->psql('postgres', "
	SET log_statement_max_length TO -1;
	SELECT '123456789ABCDEF' \\bind \\g");
ok( $node->log_contains(
		qr/execute <unnamed>: SELECT '123456789ABCDEF'/, $log_offset),
	"extended protocol -1 logs full query");

# Verify truncation applies to the parse/bind/execute duration log entries
# emitted by log_min_duration_statement.  log_statement must be 'none' to
# ensure the duration entries include the statement text.
note "Duration logging via log_min_duration_statement";
$log_offset = -s $node->logfile;
$node->psql('postgres', "
	SET log_statement TO 'none';
	SET log_min_duration_statement TO 0;
	SET log_statement_max_length TO 20;
	SELECT '123456789ABCDEF' \\bind \\g");
ok( $node->log_contains(
		qr/parse <unnamed>: SELECT '123456789ABC(?!D)/, $log_offset),
	"parse duration entry truncated");
ok( $node->log_contains(
		qr/bind <unnamed>: SELECT '123456789ABC(?!D)/, $log_offset),
	"bind duration entry truncated");
ok( $node->log_contains(
		qr/execute <unnamed>: SELECT '123456789ABC(?!D)/, $log_offset),
	"execute duration entry truncated");

$node->stop;
done_testing();
