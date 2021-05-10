<?php
function readZoneTab( string $tabFile ) : array
{
	$tab = [];

	// process extra info
	$f = file( 'code/zone.tab' );

	foreach ( $f as $line )
	{
		$line = trim( $line );
		$comments = '';

		if ( strlen( $line ) < 5 || $line[0] == '#' )
		{
			continue;
		}
		$fields = explode( "\t", $line );
		list( $countryCode, $coordinates, $tzid ) = $fields;
		if ( sizeof( $fields ) > 3 ) {
			$comments = $fields[3];
		}

		// format lang/lat
		if ( strlen( $coordinates ) == 11 )
		{
			sscanf( $coordinates, '%c%2d%2d%c%3d%2d', $xSign, $xH, $xM, $ySign, $yH, $yM );
			$xS = $yS = 0;
		}
		else
		{
			sscanf( $coordinates, '%c%2d%2d%2d%c%3d%2d%2d', $xSign, $xH, $xM, $xS, $ySign, $yH, $yM, $yS );
		}
		$lat = $xH + ( $xM / 60 ) + ( $xS / 3600 );
		$long = $yH + ( $yM / 60 ) + ( $yS / 3600 );
		$lat = $xSign == '+' ? $lat : -$lat;
		$long = $ySign == '+' ? $long : -$long;

		$tab[$tzid] = [
			'cc' => $countryCode,
			'lat' => ($lat + 90) * 100000,
			'lng' => ($long + 180) * 100000,
			'desc' => $comments ?: '',
		];
	}

	return $tab;
}

function readDataFile( string $tzid, array $tab ) : array
{
	$originalHeaderSize = 20;

	// obtain data from tz files
	$fatData  = file_get_contents( "code/data/{$tzid}",      false, NULL, $originalHeaderSize );
	$slimData = file_get_contents( "code/data-slim/{$tzid}", false, NULL, $originalHeaderSize );

	$tabData = array_key_exists( $tzid, $tab ) ? $tab[$tzid] : null;

	if ( $tabData ) {
		$header = pack( 'a4ca2a13', 'PHP2', true, $tabData['cc'], '' );
		$footer = pack( 'NNNa*', $tabData['lat'], $tabData['lng'], strlen( $tabData['desc'] ), $tabData['desc'] );
	} else {
		$header = pack( 'a4ca2a13', 'PHP2', $tzid == 'UTC', '??', '' );
		$footer = pack( 'NNNa*', 0, 0, 0, '' );
	}

	$fatData  = $header . $fatData  . $footer;
	$slimData = $header . $slimData . $footer;

	return [ $fatData, $slimData ];
}

function createDataAndIndex(array $tab)
{
	$files = array_merge(glob("code/data/*"), glob("code/data/*/*"), glob("code/data/*/*/*"));
	usort($files, 'strcasecmp');

	$fatIndex = [];
	$slimIndex = [];
	$fatData = $slimData = '';

	foreach ($files as $fileName) {
		if (is_dir($fileName)) {
			continue;
		}

		$tzid = preg_replace('@code/data/@', '', $fileName);
		list($tzFatData, $tzSlimData) = readDataFile($tzid, $tab);

		$fatIndex[$tzid] = strlen($fatData);
		$slimIndex[$tzid] = strlen($slimData);

		$fatData .= $tzFatData;
		$slimData .= $tzSlimData;
	}

	return [$fatIndex, $fatData, $slimIndex, $slimData];
}

function getVersion() : string
{
	$version_info = file( 'version-info.txt' );
	$version = trim( $version_info[1] );

	return $version;
}

function addData(array $index, string $data)
{
	$elements = count( $index );
	$flipIndex = array_flip( $index );
	$dataSize = strlen( $data );

	$str = "const timelib_tzdb_index_entry timezonedb_idx_builtin[$elements] = {\n";
	foreach ( $index as $tzid => $start )
	{
		$str .= sprintf( "\t{ %-36s, 0x%06X },\n", "\"{$tzid}\"", $start );
	}
	$str .= "};\n";

	$pos = 0;

	$str .= "\n\nconst unsigned char timelib_timezone_db_data_builtin[{$dataSize}] = {";
	for ( $i = 0; $i < $dataSize; $i++ )
	{
		if ( isset( $flipIndex[$i] ) )
		{
			$str .= "\n\n/* {$flipIndex[$i] } */\n";
			$pos = 0;
		}
		$str .= sprintf( "0x%02X,", ord( $data[$i] ) );
		if ( $pos % 16 == 15) {
			$str .= "\n";
		} else {
			$str .= " ";
		}
		$pos++;
	}
	$str .= "\n};\n";

	return $str;
}

function writeTimelibH(array $fatIndex, string $fatData, array $slimIndex, string $slimData )
{
	$elements = count( $fatIndex );

	$data = "/* This is a generated file, do not modify */\n";

	/* Error out if TIMELIB_SUPPORTS_V2DATA isn't set */
	$data .= "#ifndef TIMELIB_SUPPORTS_V2DATA\n";
	$data .= "#error Your version of timelib does not understand the timzonedb data format, please upgrade\n";
	$data .= "#endif\n\n";

	$data .= "#ifdef TIMELIB_SUPPORT_SLIM_FILE\n";
	$data .= addData( $slimIndex, $slimData );
	$data .= "#else\n";
	$data .= addData( $fatIndex, $fatData );
	$data .= "#endif\n\n";

	$version = getVersion();
	$data .= "const timelib_tzdb timezonedb_builtin = { \"{$version}\", {$elements}, timezonedb_idx_builtin, timelib_timezone_db_data_builtin };\n";

	file_put_contents( 'timezonedb.h', $data );
}

$tab = readZoneTab( 'code/zone.tab' );
list( $fatIndex, $fatData, $slimIndex, $slimData ) = createDataAndIndex( $tab );
writeTimelibH( $fatIndex, $fatData, $slimIndex, $slimData );
?>
