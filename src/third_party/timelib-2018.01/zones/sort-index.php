<?php
	$idx = file("timezonedb.idx");
	usort($idx, 'sortfunc');
	echo implode($idx);

	function sortfunc($a, $b)
	{
		/* Grep tz names */
		preg_match('@"([^"]+)"@', $a, $ma);
		$na = $ma[1];
		preg_match('@"([^"]+)"@', $b, $mb);
		$nb = $mb[1];
		
		$val = strcasecmp($na, $nb);
		return $val;
	}
?>
