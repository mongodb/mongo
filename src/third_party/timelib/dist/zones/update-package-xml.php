<?php
$e = file_get_contents( $argv[1] . '/package.xml' );

/* Grab version info */
$version_info = file( 'version-info.txt' );
$tzversion = trim( $version_info[0] );
$version = trim( $version_info[1] );

/* Use preg_replace to update version in package.xml */
$f = preg_replace( '@release>20.*</release>@', "release>{$version}</release>", $e );
$g = preg_replace( '@api>20.*</api>@', "api>{$version}</api>", $f );
$h = preg_replace( '@version 20.*@', "version {$version} ({$tzversion})", $g );
$date = date( 'Y-m-d' );
$i = preg_replace( '@date>.*</date@', "date>{$date}</date", $h );

file_put_contents( $argv[1] . '/package.xml', $i );
?>
