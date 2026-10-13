<?php
/*
 * Optional debug log of the requests the game and game servers send.
 *
 * Off by default. Turn it on with $log_requests = 1 in _ccconfig.php. Logs go
 * to $log_dir (default ../logs/requests/, i.e. outside the web root) because
 * they contain players' auth tokens and raw round data.
 */

// Same file the endpoints load; require_once makes sure it is only loaded
// once, into the global scope
require_once(dirname(__FILE__) . '/_ccconfig.php');

function logRequest($name, $body = null) {
    global $log_requests, $log_dir;
    if (empty($log_requests)) {
        return;
    }

    $dir = (isset($log_dir) && $log_dir !== '') ? $log_dir : '../logs/requests/';
    // Relative paths are relative to the web root, as elsewhere in the config
    if (!preg_match('#^([A-Za-z]:)?[/\\\\]#', $dir)) {
        $dir = dirname(dirname(__FILE__)) . '/' . $dir;
    }
    $dir = rtrim($dir, '/\\') . '/';
    if (!is_dir($dir) && !@mkdir($dir, 0777, true)) {
        return;
    }

    $ip = isset($_SERVER['REMOTE_ADDR']) ? $_SERVER['REMOTE_ADDR'] : '-';
    $query = isset($_SERVER['QUERY_STRING']) ? $_SERVER['QUERY_STRING'] : '';
    $entry = date('Y-m-d H:i:s') . ' ' . $ip . ' ' . $name . ($query !== '' ? '?' . $query : '') . "\n";
    if ($body !== null) {
        $entry .= $body . "\n\n";
    }
    @file_put_contents($dir . preg_replace('/[^A-Za-z0-9_\-]/', '', $name) . '.log', $entry, FILE_APPEND | LOCK_EX);
}
