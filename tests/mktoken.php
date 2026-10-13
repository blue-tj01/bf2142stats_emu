<?php
// Prints a game auth token. Usage: php mktoken.php WEB_DIR PID [IS_SERVER] [AGE_SECONDS]
chdir($argv[1]);
require 'ea_support.php';
$pid = (int) $argv[2];
$isServer = isset($argv[3]) ? (int) $argv[3] : 0;
$age = isset($argv[4]) ? (int) $argv[4] : 0;
$bf = new ea_stats();
$code = dwh(dechex(time() - $age)) . dwh(dechex(100)) . dwh(dechex($pid)) . '0' . $isServer . '00';
$code .= CalcCRC($code);
echo $bf->getBase64Encode($bf->DefEncryptBlock($bf->hex2str($code)));
