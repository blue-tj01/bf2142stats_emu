<?php
/*
 * Battlefield 2142 stats configuration (example).
 *
 * Copy this file to _ccconfig.php and set your database details there.
 * _ccconfig.php is not tracked by git, so your settings stay out of commits.
 */
$db_expected_ver = '1.10.1';
$db_host = 'localhost';
$db_name = 'bf2142';
$db_user = 'bf2142';
$db_pass = 'change-me';


// Web admin (admin.php) login. Logging in is disabled while either is empty.
$admin_user = '';
$admin_pass = '';
// Addresses allowed to open admin.php: IPs, x.x.x.x/nn subnets or localhost
$admin_hosts = array('localhost');
$admin_log = '../logs/_admin_event.log';
$admin_backup_path = './_backups/';
$admin_backup_ext = '.php';
$admin_page_size = 25;
$admin_ignore_ai = 1;

$stats_ext = '.txt';
$stats_logs = 'logs/';
$stats_logs_store = 'logs/_processed/';
$stats_move_logs = 1;
$stats_min_game_time = 0;
$stats_min_player_game_time = 0;
$stats_players_min = 1;
$stats_players_max = 256;

// Save every game request and round snapshot to $log_dir for debugging.
// The logs contain players' auth tokens, so keep $log_dir outside the web
// root (relative paths are relative to the web root).
$log_requests = 0;
$log_dir = '../logs/requests/';

$debug_lvl = 4;
$debug_log = '../logs/_stats_errors.log';

/*
$game_hosts = array(
'127.0.0.1',
'localhost'
);
*/
$game_custom_mapid = 700;
$game_unlocks = 0;
?>