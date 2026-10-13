<?php

$timestamp = time();
$version = 115;

require_once('include/requestlog.php');
logRequest('clearranknotification');
//$_GET['auth'] = "2kug7VmGH5Tn5kUda1dNqw__";

if (isset($_GET["auth"]) AND $_GET["auth"] != "") {
    $auth = $_GET["auth"];
} else {
    echo "None: No error.";
    exit;
}


require_once("ea_support.php");
require_once('include/_ccconfig.php');
require_once('include/compat.php');

$token = requireFreshAuthToken($auth, 708);
$authPID = $token['pid'];
$clType = $token['clType'];
$isServer = $token['isServer'];

$connection = @mysql_connect($db_host, $db_user, $db_pass);

@mysql_select_db($db_name);

$query1 = "SELECT id FROM `subaccount` WHERE id='" . $authPID . "' LIMIT 1";
$result1 = mysql_query($query1) or die(mysql_error());
if (!mysql_num_rows($result1)) {
    errorcode(104);
    exit;
}

$query2 = "SELECT rnkcg FROM `playerprogress` WHERE pid='" . $authPID . "' LIMIT 1";
$result2 = mysql_query($query2) or die(mysql_error());
$row2 = mysql_fetch_assoc($result2);
$rnkcg = $row2 ? $row2['rnkcg'] : 0;
if ($rnkcg > 0) {
    // Clear the pending rank-up notification
    $query3 = "UPDATE playerprogress SET rnkcg = 0  WHERE pid='" . $authPID . "' LIMIT 1";
    $result3 = mysql_query($query3) or die(mysql_error());
}
// Answer the same way whether or not there was a notification to clear;
// previously a cleared notification got no response at all
if ($rnkcg >= 0) {
    $Out = "O\n" .
            "H\tresult\tasof\n" .
            "D\t0\t" . $timestamp . "\n" .
            "H\tversion\n" .
            "D\t" . $version . "\n";
} else {
    errorcode(104);
    exit;
}
$countOut = preg_replace('/[\t\n]/', '', $Out);
print $Out . "\n$\t" . strlen($countOut) . "\t$\n";
@mysql_close($connection);

function errorcode($errorcode=104) {
    $Out = "E\t" . $errorcode;
    $countOut = preg_replace('/[\t\n]/', '', $Out);
    print $Out . "\n$\t" . strlen($countOut) . "\t$\n";
}

?>