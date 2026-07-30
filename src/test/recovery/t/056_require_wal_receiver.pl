# Copyright (c) 2026, PostgreSQL Global Development Group

# Tests for the require_wal_receiver parameter with target_session_attrs.
#
# The parameter rejects a standby that has no live WAL receiver process
# (pg_stat_wal_receiver returns no row).

use strict;
use warnings;
use PostgreSQL::Test::Utils;
use PostgreSQL::Test::Cluster;
use Test::More;

# Primary
my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf', "listen_addresses = 'localhost'");
$primary->start;

# standby_live: normal streaming standby.
$primary->backup('backup_live');
my $standby_live = PostgreSQL::Test::Cluster->new('standby_live');
$standby_live->init_from_backup($primary, 'backup_live', has_streaming => 1);
$standby_live->append_conf('postgresql.conf', "listen_addresses = 'localhost'");
$standby_live->append_conf('postgresql.conf', "wal_retrieve_retry_interval = '60s'");
$standby_live->start;

# standby_norecv: in recovery but with no primary_conninfo
$primary->backup('backup_norecv');
my $standby_norecv = PostgreSQL::Test::Cluster->new('standby_norecv');
$standby_norecv->init_from_backup($primary, 'backup_norecv');
$standby_norecv->append_conf('postgresql.conf', "listen_addresses = 'localhost'");
$standby_norecv->set_standby_mode();
$standby_norecv->start;

# Make sure standby_live is actually streaming before we rely on it.
$standby_live->poll_query_until('postgres',
	"SELECT EXISTS (SELECT 1 FROM pg_stat_wal_receiver)")
	or die "standby_live never started a WAL receiver";

# Make sure standby_norecv is in recovery but not streaming.
is( $standby_norecv->safe_psql('postgres', 'SELECT pg_is_in_recovery()'),
	't',
	'standby_norecv is in recovery');

is( $standby_norecv->safe_psql('postgres',
		'SELECT EXISTS (SELECT 1 FROM pg_stat_wal_receiver)'),
	'f',
	'standby_norecv has no WAL receiver');

my ($stdout, $stderr);
my $port_primary       = $primary->port;
my $port_live          = $standby_live->port;
my $port_norecv        = $standby_norecv->port;

# 1. Streaming standby is accepted.
$standby_live->psql(
	'postgres',
	'SELECT inet_server_port()',
	connstr => "dbname=postgres host=localhost port=$port_live "
	  . "target_session_attrs=standby require_wal_receiver=1",
	stdout  => \$stdout,
	stderr  => \$stderr,
);
is($stdout, $port_live, "streaming standby accepted");

# 2. Standby with no WAL receiver is rejected.
$standby_norecv->psql(
	'postgres',
	'SELECT inet_server_port()',
	connstr => "dbname=postgres host=localhost port=$port_norecv "
	  . "target_session_attrs=standby require_wal_receiver=1",
	stdout  => \$stdout,
	stderr  => \$stderr,
);
like($stderr, qr/standby has no active WAL receiver/,
	"standby without WAL receiver rejected");

# 3. prefer-standby: dead standby skipped, falls back to primary.
$primary->psql(
	'postgres',
	'SELECT inet_server_port()',
	connstr => "dbname=postgres host=localhost,localhost "
	  . "port=$port_primary,$port_norecv "
	  . "target_session_attrs=prefer-standby require_wal_receiver=1",
	stdout  => \$stdout,
	stderr  => \$stderr,
);
is($stdout, $port_primary,
	"prefer-standby: skips dead standby, connects to primary");

# 4. any: dead standby skipped, connects to primary.
$primary->psql(
	'postgres',
	'SELECT inet_server_port()',
	connstr => "dbname=postgres host=localhost,localhost "
	  . "port=$port_norecv,$port_primary "
	  . "target_session_attrs=any require_wal_receiver=1",
	stdout  => \$stdout,
	stderr  => \$stderr,
);
is($stdout, $port_primary,
	"any: skips dead standby, connects to primary");

# 5. any: dead standby skipped, live standby accepted.
$standby_live->psql(
	'postgres',
	'SELECT inet_server_port()',
	connstr => "dbname=postgres host=localhost,localhost "
	  . "port=$port_norecv,$port_live "
	  . "target_session_attrs=any require_wal_receiver=1",
	stdout  => \$stdout,
	stderr  => \$stderr,
);
is($stdout, $port_live,
	"any: skips dead standby, connects to live standby");

# 6. read-only: dead standby skipped, live standby accepted (primary is
#    read-write so skipped by the read-only filter).
$standby_live->psql(
	'postgres',
	'SELECT inet_server_port()',
	connstr => "dbname=postgres host=localhost,localhost,localhost "
	  . "port=$port_primary,$port_norecv,$port_live "
	  . "target_session_attrs=read-only require_wal_receiver=1",
	stdout  => \$stdout,
	stderr  => \$stderr,
);
is($stdout, $port_live,
	"read-only: skips dead standby, connects to live standby");

# 7. standby only, all standbys dead: connection fails.
$standby_norecv->psql(
	'postgres',
	'SELECT inet_server_port()',
	connstr => "dbname=postgres host=localhost port=$port_norecv "
	  . "target_session_attrs=standby require_wal_receiver=1",
	stdout  => \$stdout,
	stderr  => \$stderr,
);
like($stderr, qr/standby has no active WAL receiver/,
	"standby-only with no live standby fails");

# 8. read-write: check is bypassed entirely; connects to primary even when a
#    dead standby is listed first.
$primary->psql(
	'postgres',
	'SELECT inet_server_port()',
	connstr => "dbname=postgres host=localhost,localhost "
	  . "port=$port_norecv,$port_primary "
	  . "target_session_attrs=read-write require_wal_receiver=1",
	stdout  => \$stdout,
	stderr  => \$stderr,
);
is($stdout, $port_primary,
	"read-write: check bypassed, connects to primary");

# 9. primary: connecting directly to a primary with the check enabled is fine
#    (pg_is_in_recovery() is false, so require_wal_receiver is ignored).
$primary->psql(
	'postgres',
	'SELECT inet_server_port()',
	connstr => "dbname=postgres host=localhost port=$port_primary "
	  . "require_wal_receiver=1",
	stdout  => \$stdout,
	stderr  => \$stderr,
);
is($stdout, $port_primary,
	"primary accepted (check short-circuits on non-recovery server)");

# 10. Invalid boolean value is rejected during option parsing.
$standby_live->psql(
	'postgres',
	'SELECT 1',
	connstr => "dbname=postgres host=localhost port=$port_live "
	  . "require_wal_receiver=foo",
	stdout  => \$stdout,
	stderr  => \$stderr,
);
like($stderr, qr/invalid require_wal_receiver value/,
	"invalid boolean value rejected");

# 11. Upstream lost: after the primary stops, standby_live's WAL receiver exits
#     and (thanks to the long retry interval) stays gone, so the host is
#     rejected.  Run last because it stops the primary.
$primary->stop;
$standby_live->poll_query_until('postgres',
	"SELECT NOT EXISTS (SELECT 1 FROM pg_stat_wal_receiver)")
	or die "standby_live WAL receiver did not exit after primary stop";

$standby_live->psql(
	'postgres',
	'SELECT inet_server_port()',
	connstr => "dbname=postgres host=localhost port=$port_live "
	  . "target_session_attrs=standby require_wal_receiver=1",
	stdout  => \$stdout,
	stderr  => \$stderr,
);
like($stderr, qr/standby has no active WAL receiver/,
	"standby rejected after losing upstream");

done_testing();