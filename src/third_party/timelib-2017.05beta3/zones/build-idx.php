<?php
$indexFileName = 'timezonedb.idx';
$indexPhpFileName = 'timezonedb.idx.php';
$dataFileName = 'timezonedb.dta';

file_put_contents( $dataFileName, '' );

$idx_php = "<?php return [\n";
$idx_data = <<<HEREDOC
#ifdef TIMELIB_SUPPORTS_V2DATA
# define FOR_V2(v2,v1) v2
#else
# define FOR_V2(v2,v1) v1
#endif

HEREDOC;
$currentDataLength = 0;
$missing = 0;

$files = array_merge( glob( "code/data/*" ), glob( "code/data/*/*" ), glob( "code/data/*/*/*" ) );
usort( $files, 'strcasecmp' );
foreach( $files as $fileName )
{
	if ( is_dir( $fileName ) )
	{
		continue;
	}

	$l = filesize( $dataFileName );
	$fileName = preg_replace( '@code/data/@', '', $fileName );

	list( $fdataSize, $v2Start, $v2Size ) = explode( ';', `php create-entry.php $fileName $dataFileName` );

	$idx_data .= sprintf( "\t{ %-36s, FOR_V2(0x%06X, 0x%06X) },\n", '"' . $fileName. '"', $l, $l - $missing );
	$idx_php .= sprintf(
		"\t%-6d => [ 'key' => %-32s 'dsize' => 0x%08X, 'v2end' => 0x%08X ],\n",
		$l,
		"'{$fileName}',",
		$fdataSize,
		$v2Start + $v2Size
	);

	$missing += $v2Size;
	printf("- %s (%d, %d)\n", $fileName, $l, $missing );
}

$idx_php .= "]; ?>\n";

file_put_contents( $indexFileName, $idx_data );
file_put_contents( $indexPhpFileName, $idx_php );

echo "Done\n";
?>
