<?php
$index_data = "/* This is a generated file, do not modify */\n";
$header_data = '';
$data = '';

/* Grab version info */
$version_info = file( 'version-info.txt' );
$version = trim( $version_info[1] );

$idx_entries = count( file( 'timezonedb.idx' ) ) - 5;
$index_data .= "const timelib_tzdb_index_entry timezonedb_idx_builtin[{$idx_entries}] = {\n";
$index_data .= file_get_contents( 'timezonedb.idx' );
$index_data .= "};\n";

$index = include 'timezonedb.idx.php';
$dta = file_get_contents('timezonedb.dta');
$dta_l = strlen($dta);
$legacy_dta_l = $dta_l;

$j = 0;
$ascii = '';

for ($i = 0; $i < $dta_l; $i++)
{
	if ( array_key_exists( $i, $index ) )
	{
		/* Find marker for V2 fields */
		$v2 = strpos( $dta, "TZif", $i );
		/* Find end of data */
		$endOfData = $i + $index[$i]['v2end'];

		$legacy_dta_l -= ( $endOfData - $v2 );

		if ( $i != 0 && $ascii !== '' )
		{
			$data .= str_repeat( ' ', 6 * ( 16 - ( $j % 16 ) ) );
			$data .= " /* {$ascii} */\n";
		}
		$data .= "\n\n/* {$index[$i]['key']} */\n";
		$j = 0; $ascii = '';
		echo ".";
	}
	if ($j % 16 != 0) {
		$data .= " ";
	}
	$char = ord( $dta[$i] );
	$data .= sprintf( "0x%02X,", $char );

	if ( $argv[1] == 'debug' )
	{
		$ascii .= ($char >= 32 && $char != 47 && $char < 127) ? sprintf( "%c", $char ) : '.';
	}

	if ( $j % 16 == 15 )
	{
		if ( $ascii !== '' )
		{
			$data .= " /* {$ascii} */\n";
		}
		else
		{
			$data .= "\n";
		}
		$ascii = '';
	}
	$j++;

	if ( $i + 1 == $v2 )
	{
		if ( $ascii !== '' )
		{
			$data .= str_repeat( ' ', 6 * ( 16 - ( $j % 16 ) ) );
			$data .= " /* {$ascii} */\n";
		}
		else if ( $j % 16 != 0 )
		{
			$data .= "\n";
		}
		$ascii = '';
		$data .= "#ifdef TIMELIB_SUPPORTS_V2DATA\n";
		$j = 0;
	}
	if ( $i + 1 == $endOfData )
	{
		if ( $ascii !== '' )
		{
			$data .= str_repeat( ' ', 6 * ( 16 - ( $j % 16 ) ) );
			$data .= " /* {$ascii} */\n";
		}
		else if ( $j % 16 != 0 )
		{
			$data .= "\n";
		}
		$ascii = '';
		$data .= "#endif\n";
		$j = 0;
	}
}
$data .= "};\n";
echo "\n";

$data .= "\n";

$header_data .= "#ifdef TIMELIB_SUPPORTS_V2DATA\n" .
	"const unsigned char timelib_timezone_db_data_builtin[$dta_l] = {\n" .
	"#else\n" .
	"const unsigned char timelib_timezone_db_data_builtin[$legacy_dta_l] = {\n" .
	"#endif\n";

$footer_data .= "const timelib_tzdb timezonedb_builtin = { \"{$version}\", {$idx_entries}, timezonedb_idx_builtin, timelib_timezone_db_data_builtin };\n";

file_put_contents( 'timezonedb.h', $index_data . $header_data . $data . $footer_data );
?>
