<?php

// No Direct Access
defined( '_BF2142_ADMIN' ) or die( 'Restricted access' );

// Start Session
start_session();

function start_session(){
   static $started = false;
   if(!$started){
       session_start();
       $started = true;
   }
}

function checkSession() {
	global $cfg;
	// Check Session Values
	if (!isset($_SESSION['adminAuth'])) {
		return false;
	} elseif (($_SESSION['adminAuth']) != md5($cfg->get('admin_user').$cfg->get('admin_pass'))) {
		return false;
	} elseif ($_SESSION['adminTime'] < time() - (30*60)) {	// Session Older tha n 30 minutes
		return false;
	} else {
		// Update Session Time
		$_SESSION['adminTime'] = time();
		return true;
	}
}

// Failed login tracking is kept server-side per client IP, so it can't be reset
// by the client (previously the attempt counter was a hidden form field).
define('_ADMIN_MAX_ATTEMPTS', 5);
define('_ADMIN_LOCKOUT_SECS', 15 * 60);

function loginAttemptsFile() {
	$ip = isset($_SERVER['REMOTE_ADDR']) ? $_SERVER['REMOTE_ADDR'] : 'unknown';
	return rtrim(sys_get_temp_dir(), '/\\') . DIRECTORY_SEPARATOR . 'bf2142_admin_' . md5($ip);
}

function getLoginAttempts() {
	$file = loginAttemptsFile();
	if (!is_file($file)) {
		return array('count' => 0, 'time' => 0);
	}
	$data = @unserialize(@file_get_contents($file));
	if (!is_array($data) || !isset($data['count'], $data['time']) || $data['time'] < time() - _ADMIN_LOCKOUT_SECS) {
		return array('count' => 0, 'time' => 0);
	}
	return $data;
}

function recordFailedLogin() {
	$data = getLoginAttempts();
	$data['count']++;
	$data['time'] = time();
	@file_put_contents(loginAttemptsFile(), serialize($data), LOCK_EX);
	return $data['count'];
}

function clearFailedLogins() {
	@unlink(loginAttemptsFile());
}

// Constant-time string comparison (hash_equals() needs PHP >= 5.6)
function safeStrEquals($a, $b) {
	$a = (string) $a;
	$b = (string) $b;
	if (strlen($a) != strlen($b)) {
		return false;
	}
	$diff = 0;
	for ($i = 0; $i < strlen($a); $i++) {
		$diff |= ord($a[$i]) ^ ord($b[$i]);
	}
	return $diff === 0;
}

function processLogin() {
	global $cfg;
	$formUser = !isset($_POST['formUser']) ? '' : $_POST['formUser'];
	$formPassword = !isset($_POST['formPassword']) ? '' : $_POST['formPassword'];

	$attempts = getLoginAttempts();
	if ($attempts['count'] >= _ADMIN_MAX_ATTEMPTS) {
		ErrorLog("Admin login locked out (IP: " . $_SERVER['REMOTE_ADDR'] . ")", 0);
		echo "<p align='center' style=\"font-weight:bold;color:red;font-family:sans-serif;\">Too many failed logins. Try again later.</p>";
		exit;
	}

	// Refuse to log anyone in while no admin credentials are configured
	if ($cfg->get('admin_user') == '' || $cfg->get('admin_pass') == ''
		|| !safeStrEquals($formUser, $cfg->get('admin_user'))
		|| !safeStrEquals($formPassword, $cfg->get('admin_pass'))) {
		$count = recordFailedLogin();
		ErrorLog("Admin login failed ({$count}/" . _ADMIN_MAX_ATTEMPTS . ") (IP: " . $_SERVER['REMOTE_ADDR'] . ")", 0);
		return;
	}

	clearFailedLogins();
	start_session();
	session_regenerate_id(true);
	$_SESSION['adminAuth'] = md5($cfg->get('admin_user').$cfg->get('admin_pass'));
	$_SESSION['adminTime'] = time();
	$_POST['task'] = 'home';
}

function processLogout() {
	if (!checkSession()) {
		return;
	}

	// Clear all session data and destroy the session
	// (session_is_registered()/session_unregister() were removed in PHP 5.4)
	$_SESSION = array();
	if (ini_get('session.use_cookies')) {
		$params = session_get_cookie_params();
		setcookie(session_name(), '', time() - 42000, $params['path'], $params['domain'], $params['secure'], $params['httponly']);
	}
	session_destroy();
}
?>