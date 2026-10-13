<?php
/*
 * Stats server web admin.
 *
 * Set $admin_user and $admin_pass in include/_ccconfig.php to enable logging
 * in. Only addresses in $admin_hosts may open this page; when it isn't set,
 * only this machine can.
 */
define('_BF2142_ADMIN', 1);

require('include/utils.php');
$cfg = new Config();

// Admin pages read these without checking
foreach (array('task', 'action') as $key) {
    if (!isset($_GET[$key])) {
        $_GET[$key] = '';
    }
    if (!isset($_POST[$key])) {
        $_POST[$key] = '';
    }
}

// --- Who may open the admin -------------------------------------------------

function adminHostAllowed($ip, $hosts) {
    foreach ($hosts as $host) {
        $host = trim($host);
        if ($host === $ip) {
            return true;
        }
        if ($host == 'localhost' && ($ip == '127.0.0.1' || $ip == '::1')) {
            return true;
        }
        if (isIpInNetArray($ip, array($host))) {
            return true;
        }
    }
    return false;
}

$adminHosts = $cfg->get('admin_hosts');
if (!is_array($adminHosts) || !count($adminHosts)) {
    $adminHosts = array('localhost');
}
$remoteIp = isset($_SERVER['REMOTE_ADDR']) ? $_SERVER['REMOTE_ADDR'] : '';
if (!adminHostAllowed($remoteIp, $adminHosts)) {
    header('HTTP/1.0 403 Forbidden');
    ErrorLog("Admin access refused (IP: {$remoteIp})", 0);
    die('Access denied. Add your IP address to $admin_hosts in include/_ccconfig.php.');
}

// --- Session and CSRF protection --------------------------------------------

ini_set('session.use_strict_mode', '1');
ini_set('session.cookie_httponly', '1');
ini_set('session.cookie_samesite', 'Strict');
session_name('bf2142admin');
require('include/admin.security.php');

function adminNewCsrf() {
    $_SESSION['csrf'] = function_exists('random_bytes') ? bin2hex(random_bytes(16)) : md5(uniqid(mt_rand(), true));
}
if (empty($_SESSION['csrf'])) {
    adminNewCsrf();
}
if ($_SERVER['REQUEST_METHOD'] == 'POST') {
    $sent = isset($_POST['csrf']) ? (string) $_POST['csrf'] : '';
    if (!safeStrEquals($sent, $_SESSION['csrf'])) {
        header('HTTP/1.0 400 Bad Request');
        die('Your session expired or the form was not sent from this page. Go back, reload and try again.');
    }
}

// Every form on the admin pages gets the CSRF token
function adminAddCsrf($html) {
    $field = '<input type="hidden" name="csrf" value="' . htmlspecialchars($_SESSION['csrf']) . '">';
    return preg_replace('/(<form\b[^>]*>)/i', '$1' . $field, $html);
}
ob_start('adminAddCsrf');

// --- Login / logout ----------------------------------------------------------

$action = $_POST['action'] ? $_POST['action'] : $_GET['action'];
$loginFailed = false;
if ($action == 'login') {
    processLogin();
    $loginFailed = !checkSession();
} elseif ($action == 'logout') {
    processLogout();
    // Logging out destroys the session; start a fresh one for the login form
    if (session_status() != PHP_SESSION_ACTIVE) {
        session_start();
    }
    adminNewCsrf();
}
$dbver = getDbVer();
$adminConfigured = ($cfg->get('admin_user') != '' && $cfg->get('admin_pass') != '');
?>
<!DOCTYPE html>
<html>
<head>
    <meta charset="iso-8859-1">
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <title>BF2142 Stats Admin</title>
    <link rel="stylesheet" href="include/admin.css">
    <script src="include/admin.script.js"></script>
</head>
<body>
    <header class="topbar">BF2142 Stats Admin</header>
    <div class="layout">
        <nav class="sidebar">
            <?php require('include/admin.menu.php'); ?>
        </nav>
        <main class="main">
            <?php if (!$adminConfigured) { ?>
            <div class="notice">
                Logging in is disabled until you set <code>$admin_user</code> and <code>$admin_pass</code>
                in <code>include/_ccconfig.php</code>.
            </div>
            <?php } elseif ($loginFailed) { ?>
            <div class="notice">Wrong username or password.</div>
            <?php } ?>
            <?php
            if ($action == 'process' && checkSession()) {
                require('include/admin.process.php');
            } else {
                require('include/admin.content.php');
            }
            ?>
        </main>
    </div>
</body>
</html>
