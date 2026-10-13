<?php
// Router for PHP's built-in server: run .aspx files as PHP, like web/.htaccess
// does on Apache. Usage: php -S 127.0.0.1:8088 -t web tests/router.php
$path = parse_url($_SERVER['REQUEST_URI'], PHP_URL_PATH);
$file = $_SERVER['DOCUMENT_ROOT'] . $path;
if (preg_match('/\.aspx$/', $path) && is_file($file)) {
    chdir(dirname($file));
    require $file;
    return true;
}
return false;
