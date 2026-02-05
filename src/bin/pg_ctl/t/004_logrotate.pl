
# Copyright (c) 2021-2026, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(usleep);

# Extract the file name of a $format from the contents of
# current_logfiles.
sub fetch_file_name
{
	my $logfiles = shift;
	my $format = shift;
	my @lines = split(/\n/, $logfiles);
	my $filename = undef;
	foreach my $line (@lines)
	{
		if ($line =~ /$format (.*)$/gm)
		{
			$filename = $1;
		}
	}

	return $filename;
}

# Check for a pattern in the logs associated to one format.
sub check_log_pattern
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	my $format = shift;
	my $logfiles = shift;
	my $pattern = shift;
	my $node = shift;
	my $lfname = fetch_file_name($logfiles, $format);

	my $max_attempts = 10 * $PostgreSQL::Test::Utils::timeout_default;

	my $logcontents;
	for (my $attempts = 0; $attempts < $max_attempts; $attempts++)
	{
		$logcontents = slurp_file($node->data_dir . '/' . $lfname);
		last if $logcontents =~ m/$pattern/;
		usleep(100_000);
	}

	like($logcontents, qr/$pattern/,
		"found expected log file content for $format");

	# While we're at it, test pg_current_logfile() function
	is( $node->safe_psql('postgres', "SELECT pg_current_logfile('$format')"),
		$lfname,
		"pg_current_logfile() gives correct answer with $format");
	return;
}

# Set up node with logging collector
my $node = PostgreSQL::Test::Cluster->new('primary');
$node->init();
$node->append_conf(
	'postgresql.conf', qq(
logging_collector = on
log_destination = 'stderr, csvlog, jsonlog'
# these ensure stability of test results:
log_rotation_age = 0
lc_messages = 'C'
));

$node->start();

# Verify that log output gets to the file

$node->psql('postgres', 'SELECT 1/0');

# might need to retry if logging collector process is slow...
my $max_attempts = 10 * $PostgreSQL::Test::Utils::timeout_default;

my $current_logfiles;
for (my $attempts = 0; $attempts < $max_attempts; $attempts++)
{
	eval {
		$current_logfiles = slurp_file($node->data_dir . '/current_logfiles');
	};
	last unless $@;
	usleep(100_000);
}
die $@ if $@;

note "current_logfiles = $current_logfiles";

like(
	$current_logfiles,
	qr|^stderr log/postgresql-.*log
csvlog log/postgresql-.*csv
jsonlog log/postgresql-.*json$|,
	'current_logfiles is sane');

check_log_pattern('stderr', $current_logfiles, 'division by zero', $node);
check_log_pattern('csvlog', $current_logfiles, 'division by zero', $node);
check_log_pattern('jsonlog', $current_logfiles, 'division by zero', $node);

# Sleep 2 seconds and ask for log rotation; this should result in
# output into a different log file name.
sleep(2);
$node->logrotate();

# pg_ctl logrotate doesn't wait for rotation request to be completed.
# Allow a bit of time for it to happen.
my $new_current_logfiles;
for (my $attempts = 0; $attempts < $max_attempts; $attempts++)
{
	$new_current_logfiles = slurp_file($node->data_dir . '/current_logfiles');
	last if $new_current_logfiles ne $current_logfiles;
	usleep(100_000);
}

note "now current_logfiles = $new_current_logfiles";

like(
	$new_current_logfiles,
	qr|^stderr log/postgresql-.*log
csvlog log/postgresql-.*csv
jsonlog log/postgresql-.*json$|,
	'new current_logfiles is sane');

# Verify that log output gets to this file, too
$node->psql('postgres', 'fee fi fo fum');

check_log_pattern('stderr', $new_current_logfiles, 'syntax error', $node);
check_log_pattern('csvlog', $new_current_logfiles, 'syntax error', $node);
check_log_pattern('jsonlog', $new_current_logfiles, 'syntax error', $node);

# Verify truncation works with ASCII
$node->append_conf('postgresql.conf', "log_statement = 'all'\nlog_statement_max_length = 20\n");
$node->reload();
$node->psql('postgres', "SELECT '123456789ABCDEF'");
check_log_pattern('stderr',  $new_current_logfiles, "SELECT '123456789ABC", $node);
check_log_pattern('csvlog',  $new_current_logfiles, "SELECT '123456789ABC", $node);
check_log_pattern('jsonlog', $new_current_logfiles, "SELECT '123456789ABC", $node);

# Verify -1 disables truncation (logs full query)
$node->append_conf('postgresql.conf', "log_statement_max_length = -1\n");
$node->reload();
$node->psql('postgres', "SELECT '123456789ABCDEF'");
check_log_pattern('stderr',  $new_current_logfiles, "SELECT '123456789ABCDEF'", $node);
check_log_pattern('csvlog',  $new_current_logfiles, "SELECT '123456789ABCDEF'", $node);
check_log_pattern('jsonlog', $new_current_logfiles, "SELECT '123456789ABCDEF'", $node);

# Verify multibyte character handling (emoji shouldn't be split)
$node->append_conf('postgresql.conf', "log_statement_max_length = 12\n");
$node->reload();
$node->psql('postgres', "SELECT '🐘test'");
# Should truncate to "SELECT '🐘" (12 bytes) or "SELECT '" (8 bytes) if emoji doesn't fit
# But should NOT have broken UTF-8
check_log_pattern('stderr',  $new_current_logfiles, "SELECT '", $node);
check_log_pattern('csvlog',  $new_current_logfiles, "SELECT '", $node);
check_log_pattern('jsonlog', $new_current_logfiles, "SELECT '", $node);

$node->stop();

done_testing();
