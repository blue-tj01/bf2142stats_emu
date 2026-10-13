<?php
/*
 * Keeps game servers that are registered by DNS name authorised.
 *
 * For each row in `servers` with `bydnsname` = 1, resolves `dnshost` and, if
 * the address changed, updates `ip` so stats from that server keep being
 * accepted. Useful for game servers on a dynamic IP.
 *
 * Run it regularly, e.g. every 5 minutes from cron or Task Scheduler:
 *   php /path/to/web/dnsch.php
 * Over HTTP it only answers requests from this machine.
 */

// Paths in the config are relative to the web root
chdir(dirname(__FILE__));

require('include/utils.php');
$cfg = new Config();

$isCli = (PHP_SAPI == 'cli');
if (!$isCli) {
    $remote = isset($_SERVER['REMOTE_ADDR']) ? $_SERVER['REMOTE_ADDR'] : '';
    if ($remote != '127.0.0.1' && $remote != '::1') {
        header('HTTP/1.0 403 Forbidden');
        ErrorLog("dnsch.php: refused request from {$remote}", 0);
        exit;
    }
    header('Content-Type: text/plain');
}

$connection = @mysql_connect($cfg->get('db_host'), $cfg->get('db_user'), $cfg->get('db_pass'));
if (!$connection || !@mysql_select_db($cfg->get('db_name'), $connection)) {
    $errmsg = "dnsch.php: can't connect to the database: " . mysql_error();
    ErrorLog($errmsg, 1);
    echo $errmsg . "\n";
    exit(1);
}

$query = "SELECT id, ip, dnshost, prefix FROM servers WHERE bydnsname = 1 AND dnshost <> ''";
$result = mysql_query($query);
if (checkSQLResult($result, $query)) {
    echo "Database error, see the error log\n";
    exit(1);
}

$changed = 0;
while ($server = mysql_fetch_assoc($result)) {
    $host = $server['dnshost'];
    $newip = gethostbyname($host);
    if ($newip == $host || !preg_match('/^\d{1,3}(\.\d{1,3}){3}$/', $newip)) {
        ErrorLog("dnsch.php: can't resolve {$host}", 2);
        echo "{$host}: can't resolve\n";
        continue;
    }
    if ($newip == $server['ip']) {
        echo "{$host}: unchanged ({$newip})\n";
        continue;
    }

    $id = (int) $server['id'];
    $ip = mysql_real_escape_string($newip);
    $prefix = mysql_real_escape_string($server['prefix']);

    // `servers` has a unique key on (ip, prefix): drop any other row that
    // already holds the new address
    $query = "DELETE FROM servers WHERE ip = '{$ip}' AND prefix = '{$prefix}' AND id <> {$id}";
    checkSQLResult(mysql_query($query), $query);

    $query = "UPDATE servers SET ip = '{$ip}' WHERE id = {$id}";
    if (!checkSQLResult(mysql_query($query), $query)) {
        ErrorLog("dnsch.php: {$host} changed {$server['ip']} => {$newip}", -1);
        echo "{$host}: {$server['ip']} => {$newip}\n";
        $changed++;
    }
}
echo "Done, {$changed} server(s) updated\n";
@mysql_close($connection);
