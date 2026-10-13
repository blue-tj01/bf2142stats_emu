<?php
// Writes DB_HOST / DB_NAME / DB_USER / DB_PASS from the environment into the
// stats config. Variables that aren't set leave the config value unchanged.
$file = $argv[1];
$cfg = file_get_contents($file);
$map = array('db_host' => 'DB_HOST', 'db_name' => 'DB_NAME', 'db_user' => 'DB_USER', 'db_pass' => 'DB_PASS');
foreach ($map as $var => $env) {
    $value = getenv($env);
    if ($value === false) {
        continue;
    }
    $line = '$' . $var . ' = ' . var_export($value, true) . ';';
    $cfg = preg_replace_callback('/^\$' . $var . ' = .*;$/m', function () use ($line) {
        return $line;
    }, $cfg);
}
file_put_contents($file, $cfg);
