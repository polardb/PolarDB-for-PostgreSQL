#!/usr/bin/perl

# 008_proxy.pl
#	  Test PolarDB proxy, including protocol, cancel request and so on.
#
# Copyright (c) 2022, Alibaba Group Holding Limited
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# IDENTIFICATION
#	  src/test/polar_pl/t/008_proxy.pl

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use PolarDB::Task;
use Test::More skip_all => 'Temporarily disabled: unstable proxy test';

my $with_ssl = ($ENV{with_ssl} eq 'openssl');

my $dbname = 'postgres';

my $client_host = '127.0.0.1';
my $client_port = 10000;
my @client_addr = ($client_host, $client_port);

my $session_id_base = 10000000;
my $session_id = 11111111;
my $cancel_key = 999;
my @session_info = ($session_id, $cancel_key);

my $ssl_version = '1.0';
my $ssl_cipher_name = 'cipher_name';
my @ssl_info = (1, $ssl_version, $ssl_cipher_name);

my $trust_host = '1.1.1.1';
my $reject_host = '1.1.1.3';
my $trust_host_ipv6 = '2001:db8::1234';
my $reject_host_ipv6 = '2001:db8::1236';

my $pid_sql = 'select pg_backend_pid()';
my $dummy_sql = 'select 1';
my $count_proxy_sql =
  " select count(*) from pg_stat_activity where pid > $session_id_base";
my $display_sql = 'set polar_session_id_display_method=';

sub set_env
{
	my ($name, $val, $bool) = @_;
	if (defined $val)
	{
		$ENV{$name} = $bool ? ($val ? 'true' : 'false') : $val;
	}
	else
	{
		delete $ENV{$name};
	}
}

sub set_envs
{
	my ($client_host, $client_port, $session_id, $cancel_key, $send_lsn,
		$use_ssl, $ssl_version, $ssl_cipher_name)
	  = @_;
	set_env('_polar_proxy_client_host', $client_host, 0);
	set_env('_polar_proxy_client_port', $client_port, 0);
	set_env('_polar_proxy_session_id', $session_id, 0);
	set_env('_polar_proxy_cancel_key', $cancel_key, 0);
	set_env('_polar_proxy_send_lsn', $send_lsn, 1);
	set_env('_polar_proxy_use_ssl', $use_ssl, 1);
	set_env('_polar_proxy_ssl_version', $ssl_version, 0);
	set_env('_polar_proxy_ssl_cipher_name', $ssl_cipher_name, 0);
}

sub proxy_psql
{
	my ($node, $sql, @envs) = @_;
	set_envs(@envs);
	return $node->psql($dbname, $sql);
}

sub proxy_safe_psql
{
	my ($node, $sql, @envs) = @_;
	set_envs(@envs);
	return $node->safe_psql($dbname, $sql);
}

sub start_proxy_backend
{
	my ($node) = @_;
	Task::start_new_task('proxy sleep', [$node], \&proxy_psql,
		[ 'select pg_sleep(3);', @client_addr, 19999999, $cancel_key ],
		Task::EXEC_ONCE, -3);
	while (
		$node->safe_psql($dbname,
			$display_sql . 'force_proxy;' . $count_proxy_sql) < 1)
	{
		sleep(0.1);
	}
}

# Initialize primary node
my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->init(allows_streaming => 1);
$node_primary->append_conf('postgresql.conf',
	qq{polar_enable_debug_proxy = on});
$node_primary->append_conf('pg_hba.conf',
	qq{host all all $trust_host/32 trust});
$node_primary->append_conf('pg_hba.conf',
	qq{host all all $reject_host/32 reject});
$node_primary->append_conf('pg_hba.conf',
	qq{host all all $trust_host_ipv6/128 trust});
$node_primary->append_conf('pg_hba.conf',
	qq{host all all $reject_host_ipv6/128 reject});
$node_primary->start;
my $backup_name = 'my_backup';

# Take backup
$node_primary->backup($backup_name);

# Create streaming standby linking to primary
my $node_standby = PostgreSQL::Test::Cluster->new('standby');
$node_standby->init_from_backup($node_primary, $backup_name,
	has_streaming => 1);
$node_standby->start;

# Create extension
if ($with_ssl)
{
	$node_primary->safe_psql($dbname, 'create extension sslinfo');
}
$node_primary->safe_psql($dbname, 'create user normal with login');
$node_primary->wait_for_catchup($node_standby);

# Check connection & auto generated session id
ok( proxy_safe_psql($node_primary, $pid_sql, @client_addr) < $session_id_base,
	"suerpuser primary proxy pid less than $session_id_base");
is(proxy_psql($node_primary, $pid_sql, '1.1.1.2', $client_port),
	2, 'primary proxy hba is not found');
is(proxy_psql($node_primary, $pid_sql, $reject_host, $client_port),
	2, 'primary proxy hba is rejected');
is(proxy_psql($node_primary, $pid_sql, '2001:db8::1235', $client_port),
	2, 'primary proxy hba is not found');
is(proxy_psql($node_primary, $pid_sql, $reject_host_ipv6, $client_port),
	2, 'primary proxy hba is rejected');
ok( proxy_safe_psql($node_standby, $pid_sql, @client_addr) < $session_id_base,
	'standby proxy pid not start from $session_id_base');
is(proxy_psql($node_standby, $pid_sql, '1.1.1.2', $client_port),
	2, 'standby proxy hba is not found');
is(proxy_psql($node_standby, $pid_sql, $reject_host, $client_port),
	2, 'standby proxy hba is rejected');
is(proxy_psql($node_standby, $pid_sql, '2001:db8::1235', $client_port),
	2, 'standby proxy hba is not found');
is(proxy_psql($node_standby, $pid_sql, $reject_host_ipv6, $client_port),
	2, 'standby proxy hba is rejected');

if (!$use_unix_sockets)
{
	my $from_my_activity =
	  ' from pg_stat_activity where pid = pg_backend_pid()';
	is( proxy_safe_psql(
			$node_primary, 'select client_addr' . $from_my_activity,
			$trust_host, $client_port),
		$trust_host,
		'primary proxy ip is set');
	is( proxy_safe_psql(
			$node_primary, 'select client_addr' . $from_my_activity,
			$trust_host_ipv6, $client_port),
		$trust_host_ipv6,
		'primary proxy ip is set ipv6');
	is( proxy_safe_psql(
			$node_primary, 'select client_port' . $from_my_activity,
			@client_addr),
		$client_port,
		'primary proxy port is set');
	is( proxy_safe_psql(
			$node_standby, 'select client_addr' . $from_my_activity,
			$trust_host, $client_port),
		$trust_host,
		'standby proxy ip is set');
	is( proxy_safe_psql(
			$node_standby, 'select client_addr' . $from_my_activity,
			$trust_host_ipv6, $client_port),
		$trust_host_ipv6,
		'standby proxy ip is set ipv6');
	is( proxy_safe_psql(
			$node_standby, 'select client_port' . $from_my_activity,
			@client_addr),
		$client_port,
		'standby proxy port is set');
}

## fault injection
is(proxy_psql($node_primary, $pid_sql, $trust_host, 'xxxx'),
	2, 'primary proxy invalid port 1');
is(proxy_psql($node_primary, $pid_sql, $trust_host, 0),
	2, 'primary proxy invalid port 2');
is(proxy_psql($node_primary, $pid_sql, $trust_host, 65536),
	2, 'primary proxy invalid port 3');
is(proxy_psql($node_primary, $pid_sql, 'a.b.c.d', $client_port),
	2, 'primary proxy invalid host');
is(proxy_psql($node_primary, $pid_sql, 'a:b:c:d', $client_port),
	2, 'primary proxy invalid host ipv6');

# Check lsn
is( proxy_safe_psql($node_primary, '\lsn', @client_addr, @session_info, 0),
	'no LSN infomation',
	'primary no ready for query lsn');
is( proxy_safe_psql($node_primary, '\lsn', @client_addr, @session_info, 1),
	proxy_safe_psql(
		$node_primary, 'select pg_current_wal_lsn()',
		@client_addr, @session_info,
		1),
	'primary ready for query lsn');
is( proxy_safe_psql($node_primary, '\lsn', @client_addr, @session_info, 0),
	'no LSN infomation',
	'standby no ready for query lsn');
is( proxy_safe_psql($node_primary, '\lsn', @client_addr, @session_info, 1),
	proxy_safe_psql(
		$node_standby, 'select pg_last_wal_replay_lsn()',
		@client_addr, @session_info,
		1),
	'standby ready for query lsn');

## fault injection
is(proxy_psql($node_primary, $dummy_sql, undef, undef, undef, undef, 1),
	2, 'primary direction connection request LSN');

# Check ssl
if ($with_ssl)
{
	my @ssl_conn_prefix = (@client_addr, @session_info, 0);
	is( proxy_safe_psql(
			$node_primary, 'select ssl_is_used()',
			@ssl_conn_prefix),
		'f',
		'primary disable ssl');
	is( proxy_safe_psql(
			$node_primary, 'select ssl_version()',
			@ssl_conn_prefix, 0),
		'',
		'primary disable ssl get version');
	is( proxy_safe_psql(
			$node_primary, 'select ssl_cipher()',
			@ssl_conn_prefix, 0),
		'',
		'primary disable ssl get cipher');
	is( proxy_safe_psql(
			$node_primary, 'select ssl_is_used()',
			@ssl_conn_prefix, @ssl_info),
		't',
		'primary enable ssl');
	is( proxy_safe_psql(
			$node_primary, 'select ssl_version()',
			@ssl_conn_prefix, @ssl_info),
		$ssl_version,
		'primary enable ssl get version');
	is( proxy_safe_psql(
			$node_primary, 'select ssl_cipher()',
			@ssl_conn_prefix, @ssl_info),
		$ssl_cipher_name,
		'primary enable ssl get cipher');

	## safe to call other functions, but no check results.
	print proxy_safe_psql($node_primary, 'select ssl_client_serial()',
		@ssl_conn_prefix, @ssl_info, 'ssl_client_serial');
	print proxy_safe_psql($node_primary, 'select ssl_client_cert_present()',
		@ssl_conn_prefix, @ssl_info, 'ssl_client_cert_present');
	print proxy_safe_psql($node_primary, 'select ssl_client_dn_field(\'\')',
		@ssl_conn_prefix, @ssl_info, 'ssl_client_dn_field');
	print proxy_safe_psql($node_primary, 'select ssl_issuer_field(\'\')',
		@ssl_conn_prefix, @ssl_info, 'ssl_issuer_field');
	print proxy_safe_psql($node_primary, 'select ssl_client_dn()',
		@ssl_conn_prefix, @ssl_info, 'ssl_client_dn');
	print proxy_safe_psql($node_primary, 'select ssl_issuer_dn()',
		@ssl_conn_prefix, @ssl_info, 'ssl_issuer_dn');
	print proxy_safe_psql($node_primary, 'select ssl_extension_info()',
		@ssl_conn_prefix, @ssl_info, 'ssl_extension_info');

	## fault injection
	is( proxy_psql(
			$node_primary, $dummy_sql, undef, undef, @session_info, 0, 1),
		2,
		'primary direction connection enable ssl');
	is( proxy_psql(
			$node_primary, $dummy_sql, @client_addr,
			@session_info, @ssl_info, undef),
		2,
		'primary direction connection enable ssl missing cipher');
	is( proxy_psql(
			$node_primary, $dummy_sql, @client_addr, @session_info,
			0, 1, undef, 'dummy'),
		2,
		'primary direction connection enable ssl missing version');
}

# polar_session_id_display_method
## self
local $ENV{PGUSER} = "normal";
ok( proxy_safe_psql(
		$node_primary, $display_sql . 'local;' . $pid_sql,
		@client_addr, @session_info) < $session_id_base,
	'normal display method local from self');
ok( proxy_safe_psql(
		$node_primary, $display_sql . 'proxy;' . $pid_sql,
		@client_addr, @session_info) > $session_id_base,
	'normal display method proxy from self');
ok( proxy_safe_psql(
		$node_primary, $display_sql . 'force_proxy;' . $pid_sql,
		@client_addr, @session_info) > $session_id_base,
	'normal display method force proxy from self');

delete $ENV{PGUSER};
ok( proxy_safe_psql(
		$node_primary, $display_sql . 'local;' . $pid_sql,
		@client_addr, @session_info) < $session_id_base,
	'superuser display method local from self');
ok( proxy_safe_psql(
		$node_primary, $display_sql . 'proxy;' . $pid_sql,
		@client_addr, @session_info) < $session_id_base,
	'superuser display method proxy from self');
ok( proxy_safe_psql(
		$node_primary, $display_sql . 'force_proxy;' . $pid_sql,
		@client_addr, @session_info) > $session_id_base,
	'superuser display method force proxy from self');

## start a async conn
start_proxy_backend($node_primary);

## proxy conn
is( proxy_safe_psql(
		$node_primary,
		"select count(*) from pg_stat_get_activity(-2) where pid > $session_id_base",
		@client_addr),
	0,
	'force local from proxy connection');
is( proxy_safe_psql(
		$node_primary, $display_sql . 'local;' . $count_proxy_sql,
		@client_addr),
	0,
	'display method local from proxy connection');
is( proxy_safe_psql(
		$node_primary, $display_sql . 'proxy;' . $count_proxy_sql,
		@client_addr),
	0,
	'display method proxy from proxy connection');
is( proxy_safe_psql(
		$node_primary, $display_sql . 'force_proxy;' . $count_proxy_sql,
		@client_addr),
	2,
	'display method force_proxy from proxy connection');

## direct conn
is( proxy_safe_psql(
		$node_primary,
		"select count(*) from pg_stat_get_activity(-2) where pid > $session_id_base",
		@client_addr),
	0,
	'force local from direct connection');
is( proxy_safe_psql(
		$node_primary, $display_sql . 'local;' . $count_proxy_sql,
		@client_addr),
	0,
	'display method local from direct connection');
is( proxy_safe_psql(
		$node_primary, $display_sql . 'proxy;' . $count_proxy_sql,
		@client_addr),
	0,
	'display method proxy from direct connection');
is( proxy_safe_psql(
		$node_primary, $display_sql . 'force_proxy;' . $count_proxy_sql,
		@client_addr),
	2,
	'display method force_proxy from direct connection');

# Check proxy sid and cancel key
is( proxy_safe_psql(
		$node_primary, $display_sql . 'force_proxy;' . $pid_sql,
		@client_addr, @session_info),
	$session_id,
	'primary proxy session id');
is( proxy_safe_psql(
		$node_standby, $display_sql . 'force_proxy;' . $pid_sql,
		@client_addr, @session_info),
	$session_id,
	'standby proxy session id');
ok( proxy_safe_psql($node_primary, $display_sql . 'force_proxy;' . $pid_sql,
		undef, undef, @session_info) < $session_id_base,
	'direct mode session info not work');
is( proxy_psql($node_primary, $pid_sql, @client_addr, 200000000, $cancel_key),
	0,
	'set large proxy sid');
## fault injection
is( proxy_psql($node_primary, $pid_sql, @client_addr, 19999999, $cancel_key),
	2,
	'set same proxy sid');
is( proxy_psql($node_primary, $pid_sql, @client_addr, 10000000, $cancel_key),
	2,
	'set proxy sid too small 1');
is(proxy_psql($node_primary, $pid_sql, @client_addr, 0, $cancel_key),
	2, 'set proxy sid too small 2');
is(proxy_psql($node_primary, $pid_sql, @client_addr, $session_id, undef),
	2, 'only set session id');
is(proxy_psql($node_primary, $pid_sql, @client_addr, undef, $cancel_key),
	2, 'only set cancel key');

# Check cancel key and cancel request
is(proxy_psql($node_primary, 'select pg_cancel_backend(pg_backend_pid())'),
	3, 'direct connection cancel self');
is( proxy_psql(
		$node_primary, 'select pg_cancel_backend(pg_backend_pid())',
		@client_addr, @session_info),
	3,
	'proxy cancel self with cancel key set');
is( proxy_psql(
		$node_primary, 'select pg_cancel_backend(' . $session_id . ')',
		@client_addr, @session_info),
	3,
	'proxy cancel self by pid with cancel key set');
## Cancel another backend
proxy_safe_psql($node_primary, 'select pg_terminate_backend(19999999)',
	@client_addr);
start_proxy_backend($node_primary);
is( proxy_safe_psql(
		$node_primary, 'select pg_terminate_backend(19999999)',
		@client_addr),
	't',
	'proxy terminate another');
start_proxy_backend($node_primary);
is( proxy_safe_psql(
		$node_primary,
		$display_sql . 'local; select pg_cancel_backend(19999999)',
		@client_addr),
	't',
	'proxy cancel another display method local');
start_proxy_backend($node_primary);
is( proxy_safe_psql(
		$node_primary,
		$display_sql . 'proxy; select pg_cancel_backend(19999999)',
		@client_addr),
	't',
	'proxy cancel another display method proxy');
start_proxy_backend($node_primary);
is( proxy_safe_psql(
		$node_primary,
		$display_sql . 'force_proxy; select pg_cancel_backend(19999999)',
		@client_addr),
	't',
	'proxy cancel another display method force_proxy');

# Cancel Request Protocol
start_proxy_backend($node_primary);
is( proxy_safe_psql(
		$node_primary, $display_sql . 'force_proxy;' . $count_proxy_sql),
	1,
	'proxy connections before cancel');
$node_primary->cancel_backend(19999999, 0);
is( proxy_safe_psql(
		$node_primary, $display_sql . 'force_proxy;' . $count_proxy_sql),
	1,
	'proxy connections wrong cancel key');
$node_primary->cancel_backend(19999999, $cancel_key);
sleep 1;
is( proxy_safe_psql(
		$node_primary, $display_sql . 'force_proxy;' . $count_proxy_sql),
	0,
	'proxy connections after cancel');

# Coverage stuff
my $timed_out;
set_envs(@client_addr, @session_info);
$node_primary->psql(
	$dbname, 'select pg_sleep(3)',
	timeout => 1,
	timed_out => \$timed_out);
is($timed_out, 1, 'timeout cancel');
$node_primary->cancel_backend(0, $cancel_key);
$node_primary->cancel_backend(1, $cancel_key);
$node_primary->cancel_backend(9999999, $cancel_key);
$node_primary->cancel_backend(29999999, $cancel_key);

done_testing();
