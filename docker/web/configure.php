<?php
// Writes settings from the environment into the stats config:
// DB_HOST, DB_NAME, DB_USER, DB_PASS, LOG_REQUESTS, ADMIN_USER, ADMIN_PASS
// and ADMIN_HOSTS (comma-separated). Variables that aren't set leave the
// config value unchanged.
$file = $argv[1];
$cfg = file_get_contents($file);
$map = array(
    'db_host' => 'DB_HOST',
    'db_name' => 'DB_NAME',
    'db_user' => 'DB_USER',
    'db_pass' => 'DB_PASS',
    'log_requests' => 'LOG_REQUESTS',
    'admin_user' => 'ADMIN_USER',
    'admin_pass' => 'ADMIN_PASS',
    'admin_hosts' => 'ADMIN_HOSTS',
);
$lists = array('admin_hosts');
foreach ($map as $var => $env) {
    $value = getenv($env);
    if ($value === false) {
        continue;
    }
    if (in_array($var, $lists)) {
        $items = array_filter(array_map('trim', explode(',', $value)), 'strlen');
        $value = array_values($items);
        $line = '$' . $var . ' = array(' . implode(', ', array_map(function ($v) {
            return var_export($v, true);
        }, $value)) . ');';
    } else {
        $line = '$' . $var . ' = ' . var_export($value, true) . ';';
    }
    $cfg = preg_replace_callback('/^\$' . $var . ' = .*;$/m', function () use ($line) {
        return $line;
    }, $cfg);
}
file_put_contents($file, $cfg);
