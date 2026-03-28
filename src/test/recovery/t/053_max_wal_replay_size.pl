# Copyright (c) 2025, PostgreSQL Global Development Group

# Tests for the max_wal_replay_size parameter with target_session_attrs

use strict;
use warnings;
use PostgreSQL::Test::Utils;
use PostgreSQL::Test::Cluster;
use Test::More;

# Initialize the primary node
my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf', "listen_addresses = 'localhost'");
$primary->start;

# Create and start the first standby (will be paused later to induce lag)
my $standby1_backup = $primary->backup('my_backup1');
my $standby_node1 = PostgreSQL::Test::Cluster->new('standby1');
$standby_node1->init_from_backup($primary, 'my_backup1', has_streaming => 1);
$standby_node1->append_conf('postgresql.conf', "listen_addresses = 'localhost'");
$standby_node1->start;

# Create and start the second standby (keeps streaming normally)
my $standby2_backup = $primary->backup('my_backup2');
my $standby_node2 = PostgreSQL::Test::Cluster->new('standby2');
$standby_node2->init_from_backup($primary, 'my_backup2', has_streaming => 1);
$standby_node2->append_conf('postgresql.conf', "listen_addresses = 'localhost'");
$standby_node2->start;

# Create and start a third standby that is in recovery but has no WAL receiver
# (no primary_conninfo, no restore_command).  pg_is_in_recovery() returns true
# but pg_last_wal_receive_lsn() returns NULL, exercising the code path that
# rejects connections with "no WAL receiver active".
my $standby_node3 = PostgreSQL::Test::Cluster->new('standby3');
$standby_node3->init_from_backup($primary, 'my_backup2');
$standby_node3->append_conf('postgresql.conf', "listen_addresses = 'localhost'");
$standby_node3->set_standby_mode();
$standby_node3->start;

# Pause WAL replay on standby1 to simulate lag
$standby_node1->safe_psql('postgres', 'SELECT pg_wal_replay_pause();');

# Generate WAL activity on primary to cause replay lag on standby1
$primary->safe_psql('postgres', 'CREATE TABLE t AS SELECT generate_series(1,100000) i');
$primary->safe_psql('postgres', 'SELECT pg_switch_wal();');

# Wait until standby1 has accumulated a meaningful WAL replay backlog (> 2 MB).
# Polling avoids a fixed-duration sleep that may be too short on slow systems.
$standby_node1->poll_query_until(
    'postgres',
    "SELECT pg_wal_lsn_diff(pg_last_wal_receive_lsn(), pg_last_wal_replay_lsn()) > 2 * 1024 * 1024")
    or die "standby1 did not accumulate replay lag in time";

# Collect connection ports
my ($stdout, $stderr);
my $port_primary   = $primary->port;
my $port_standby1  = $standby_node1->port;
my $port_standby2  = $standby_node2->port;
my $port_standby3  = $standby_node3->port;

# 1. Connects to standby1 when lag is below high threshold (1GB)
$standby_node1->psql(
    'postgres',
    'SELECT inet_server_port()',
    connstr => "dbname=postgres host=localhost,localhost,localhost port=$port_primary,$port_standby1,$port_standby2 target_session_attrs=standby max_wal_replay_size=1GB",
    stdout  => \$stdout,
    stderr  => \$stderr,
);
is($stdout, $port_standby1, "Connects to standby1 with high size threshold (1GB)");

# 2. Skips standby1 due to replay size (1MB threshold), connects to standby2
$standby_node1->psql(
    'postgres',
    'SELECT inet_server_port()',
    connstr => "dbname=postgres host=localhost,localhost,localhost port=$port_primary,$port_standby1,$port_standby2 target_session_attrs=standby max_wal_replay_size=1MB",
    stdout  => \$stdout,
    stderr  => \$stderr,
);
is($stdout, $port_standby2, "Skips standby1 due to replay size, connects to standby2");

# 3. Skips standby1 due to replay size, connects to primary with prefer-standby
$standby_node1->psql(
    'postgres',
    'SELECT inet_server_port()',
    connstr => "dbname=postgres host=localhost,localhost port=$port_primary,$port_standby1 target_session_attrs=prefer-standby max_wal_replay_size=1MB",
    stdout  => \$stdout,
    stderr  => \$stderr,
);
is($stdout, $port_primary, "Connects to primary (prefer-standby) due to standby1 replay size");

# 4. Connects to primary with target_session_attrs=any (standby1 exceeds threshold)
$standby_node2->psql(
    'postgres',
    'SELECT inet_server_port()',
    connstr => "dbname=postgres host=localhost,localhost port=$port_standby1,$port_primary target_session_attrs=any max_wal_replay_size=1MB",
    stdout  => \$stdout,
    stderr  => \$stderr,
);
is($stdout, $port_primary, "Connects to primary (any) due to standby1 replay size");

# 5. All connections fail: standby1 exceeds replay size, and only standbys are allowed
$standby_node1->psql(
    'postgres',
    'SELECT inet_server_port()',
    connstr => "dbname=postgres host=localhost,localhost port=$port_primary,$port_standby1 target_session_attrs=standby max_wal_replay_size=1MB",
    stdout  => \$stdout,
    stderr  => \$stderr,
);
like($stderr, qr/WAL replay size on standby is too large/,
    "Connection rejected: replay size exceeds 1MB");

# 6. Invalid value for max_wal_replay_size (non-numeric)
$standby_node1->psql(
    'postgres',
    'SELECT inet_server_port()',
    connstr => "dbname=postgres host=localhost port=$port_standby2 max_wal_replay_size=foo",
    stdout  => \$stdout,
    stderr  => \$stderr,
);
like($stderr, qr/invalid size/i, "Rejects non-numeric max_wal_replay_size value");

# 7. Negative max_wal_replay_size value
$standby_node1->psql(
    'postgres',
    'SELECT inet_server_port()',
    connstr => "dbname=postgres host=localhost port=$port_standby2 max_wal_replay_size=-1GB",
    stdout  => \$stdout,
    stderr  => \$stderr,
);
like($stderr, qr/invalid max_wal_replay_size value/,
    "Rejects negative max_wal_replay_size value");

# 8. Replay lag equals threshold exactly (should allow connection)
my $lag_bytes;
$standby_node1->psql(
    'postgres',
    'SELECT pg_wal_lsn_diff(pg_last_wal_receive_lsn(), pg_last_wal_replay_lsn())',
    connstr => "dbname=postgres host=localhost port=$port_standby1",
    stdout  => \$lag_bytes,
);
chomp($lag_bytes);
$standby_node1->psql(
    'postgres',
    'SELECT 1',
    connstr => "dbname=postgres host=localhost port=$port_standby1 max_wal_replay_size=$lag_bytes",
    stdout  => \$stdout,
);
is($stdout, '1', "Connects when replay lag equals threshold exactly ($lag_bytes bytes)");

# 9. Replay lag exceeds threshold by 1 byte (should reject)
my $lag_low;
$standby_node1->psql(
    'postgres',
    'SELECT pg_wal_lsn_diff(pg_last_wal_receive_lsn(), pg_last_wal_replay_lsn()) - 1;',
    connstr => "dbname=postgres host=localhost port=$port_standby1",
    stdout  => \$lag_low,
);
chomp($lag_low);
$standby_node1->psql(
    'postgres',
    'SELECT 1',
    connstr => "dbname=postgres host=localhost port=$port_standby1 max_wal_replay_size=$lag_low",
    stdout  => \$stdout,
    stderr  => \$stderr,
);
like($stderr, qr/WAL replay size on standby is too large/,
    "Connection rejected: replay size exceeds max_wal_replay_size ($lag_low bytes)");

# 10. Connects to standby2 after it is freshly restarted
$standby_node2->restart;
$standby_node2->psql(
    'postgres',
    'SELECT inet_server_port()',
    connstr => "dbname=postgres host=localhost,localhost,localhost port=$port_primary,$port_standby1,$port_standby2 target_session_attrs=standby max_wal_replay_size=1MB",
    stdout  => \$stdout,
    stderr  => \$stderr,
);
is($stdout, $port_standby2, "Connects to standby2 after restart");

# 11. Standby with no active WAL receiver is rejected
# standby3 is in recovery (standby.signal present) but has no primary_conninfo
# and no restore_command, so pg_last_wal_receive_lsn() returns NULL.
$standby_node3->psql(
    'postgres',
    'SELECT inet_server_port()',
    connstr => "dbname=postgres host=localhost port=$port_standby3 max_wal_replay_size=1MB",
    stdout  => \$stdout,
    stderr  => \$stderr,
);
like($stderr, qr/no WAL receiver active/,
    "Rejects standby with no active WAL receiver");

done_testing();
