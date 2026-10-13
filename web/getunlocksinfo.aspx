<?php 


require_once('include/requestlog.php');
logRequest('getunlocksinfo');

$timestamp = time();
$authPIDAvcred = 0;
//auth=kMKtvZrBVEBeSjGYCgNgQA__
//$_GET['auth'] = "RWwVGQIuIPQatqknkDoeiQ__";

if(isset($_GET["auth"]) AND $_GET["auth"] != "") {	$auth = $_GET["auth"];	} else {	echo "None: No error.";exit;	}

require_once("ea_support.php");
require_once('include/_ccconfig.php');
require_once('include/compat.php');

// The unlock screen allows older tokens than the other endpoints
$token = requireFreshAuthToken($auth, 1708);
$authPID = $token['pid'];

if(isset($_GET["pid"]) AND $_GET["pid"] != "" AND $authPID != $_GET["pid"]) {	echo "Invalid Params";exit;	}
$connection = @mysql_connect($db_host, $db_user, $db_pass);

@mysql_select_db($db_name);
$query1 = "SELECT rnk, subaccount, unavl as avcred, unlc FROM `subaccount` s 
	LEFT JOIN `playerprogress` p ON p.pid=s.id
WHERE s.id='".$authPID."' LIMIT 1";
$result1 = mysql_query($query1) or die(mysql_error());
if (!mysql_num_rows($result1) AND $game_unlocks != 1) {
	errorcode(104);
	exit;
} else {
	$row1 = mysql_fetch_array($result1);
	if ($row1['rnk'] > ($row1['avcred']+$row1['unlc'])) {
		$row1['avcred'] = $row1['rnk']-$row1['unlc'];
		$query9 = "UPDATE `playerprogress` SET unavl='".$row1['avcred']."' WHERE pid='".$authPID."'";
		$result9 = mysql_query($query9) or die(mysql_error());
	}
	$authPIDNick = rawurldecode($row1['subaccount']);
	$authPIDAvcred = $row1['avcred'];
}


if ($game_unlocks == 0) {
	$Out = "O
H\tpid\tnick\tasof
D\t".$authPID."\t".$authPIDNick."\t".$timestamp."
H\tAvcred
D\t".$authPIDAvcred."
H\tUnlockID";
	$query2 = "SELECT * FROM `unlocks` WHERE pid='".$authPID."'";
	$result2 = mysql_query($query2) or die(mysql_error());
	if (mysql_num_rows($result2)) {
		while ($row2 = mysql_fetch_array($result2)) {
			$Out .= "\nD\t".$row2['ukit'].$row2['utree'].$row2['uorder'];
		}
	}
} elseif ($game_unlocks == 1){
	$Out = "O
H\tpid\tnick\tasof
D\t".$authPID."\t".$authPIDNick."\t".$timestamp."
H\tAvcred
D\t0
H\tUnlockID";
	$Out .= "
D\t115
D\t125
D\t215
D\t225
D\t315
D\t325
D\t415
D\t425
D\t516
D\t524";
} else {
	$Out = "O
H\tpid\tnick\tasof
D\t".$authPID."\t".$authPIDNick."\t".$timestamp."
H\tAvcred
D\t".$authPIDAvcred."
H\tUnlockID";
	
}
$countOut = preg_replace('/[\t\n]/','',$Out);
print $Out."\n$\t".strlen($countOut)."\t$\n";
@mysql_close($connection);
function errorcode($errorcode=104) {
	$Out = "E\t".$errorcode;
	$countOut = preg_replace('/[\t\n]/','',$Out);
	print $Out."\n$\t".strlen($countOut)."\t$\n";
} 
?>