<?php
/*
 * Compatibility layer for PHP 7 / PHP 8.
 *
 * The ext/mysql functions (mysql_connect, mysql_query, ...) were removed in
 * PHP 7. When they are missing, this file provides them on top of mysqli so
 * the rest of the code runs unchanged on old and modern PHP alike. On old PHP
 * builds that still have ext/mysql, nothing here is defined.
 */

// Game clients parse these responses, so PHP notices/warnings must never be
// printed into them. Errors still go to the web server's error log.
ini_set('display_errors', '0');

if (!function_exists('mysql_connect')) {

    define('MYSQL_ASSOC', MYSQLI_ASSOC);
    define('MYSQL_NUM', MYSQLI_NUM);
    define('MYSQL_BOTH', MYSQLI_BOTH);

    // PHP 8.1+ makes mysqli throw exceptions by default; the code here checks
    // return values the old ext/mysql way instead.
    mysqli_report(MYSQLI_REPORT_OFF);

    $GLOBALS['__mysql_link'] = null;

    function __mysql_link($link = null) {
        if ($link instanceof mysqli) {
            return $link;
        }
        return $GLOBALS['__mysql_link'];
    }

    function mysql_connect($host = null, $user = null, $pass = null) {
        $port = null;
        $socket = null;
        // ext/mysql accepted "host:port" and "host:/path/to/socket"
        if ($host !== null && strpos($host, ':') !== false) {
            list($host, $extra) = explode(':', $host, 2);
            if (is_numeric($extra)) {
                $port = (int) $extra;
            } else {
                $socket = $extra;
            }
        }
        $link = mysqli_connect($host, $user, $pass, '', $port, $socket);
        if (!$link) {
            trigger_error('mysql_connect(): ' . mysqli_connect_error(), E_USER_WARNING);
            return false;
        }
        // Tables are latin1, as was the old libmysql default connection charset
        mysqli_set_charset($link, 'latin1');
        // MySQL 5.7+/8 and MariaDB 10.2+ default to strict SQL modes, which
        // reject the queries here (omitted NOT NULL columns, zero dates,
        // loose GROUP BY). Use the permissive mode the code was written for.
        mysqli_query($link, "SET SESSION sql_mode = 'NO_ENGINE_SUBSTITUTION'");
        $GLOBALS['__mysql_link'] = $link;
        return $link;
    }

    function mysql_select_db($db, $link = null) {
        $link = __mysql_link($link);
        return $link ? mysqli_select_db($link, $db) : false;
    }

    function mysql_query($query, $link = null) {
        $link = __mysql_link($link);
        return $link ? mysqli_query($link, $query) : false;
    }

    function mysql_num_rows($result) {
        return ($result instanceof mysqli_result) ? mysqli_num_rows($result) : false;
    }

    function mysql_fetch_array($result, $type = MYSQL_BOTH) {
        if (!($result instanceof mysqli_result)) {
            return false;
        }
        $row = mysqli_fetch_array($result, $type);
        return $row === null ? false : $row;
    }

    function mysql_fetch_assoc($result) {
        return mysql_fetch_array($result, MYSQL_ASSOC);
    }

    function mysql_fetch_row($result) {
        return mysql_fetch_array($result, MYSQL_NUM);
    }

    function mysql_free_result($result) {
        if ($result instanceof mysqli_result) {
            mysqli_free_result($result);
        }
        return true;
    }

    function mysql_insert_id($link = null) {
        $link = __mysql_link($link);
        return $link ? mysqli_insert_id($link) : false;
    }

    function mysql_affected_rows($link = null) {
        $link = __mysql_link($link);
        return $link ? mysqli_affected_rows($link) : -1;
    }

    function mysql_error($link = null) {
        $link = __mysql_link($link);
        return $link ? mysqli_error($link) : (string) mysqli_connect_error();
    }

    function mysql_errno($link = null) {
        $link = __mysql_link($link);
        return $link ? mysqli_errno($link) : (int) mysqli_connect_errno();
    }

    function mysql_real_escape_string($string, $link = null) {
        $link = __mysql_link($link);
        if (!$link) {
            return addslashes((string) $string);
        }
        return mysqli_real_escape_string($link, (string) $string);
    }

    function mysql_get_server_info($link = null) {
        $link = __mysql_link($link);
        return $link ? mysqli_get_server_info($link) : false;
    }

    function mysql_close($link = null) {
        $link = __mysql_link($link);
        if (!$link) {
            return false;
        }
        if ($link === $GLOBALS['__mysql_link']) {
            $GLOBALS['__mysql_link'] = null;
        }
        return mysqli_close($link);
    }
}

// Removed in PHP 8; magic quotes have been gone since PHP 5.4
if (!function_exists('get_magic_quotes_gpc')) {
    function get_magic_quotes_gpc() {
        return false;
    }
}
