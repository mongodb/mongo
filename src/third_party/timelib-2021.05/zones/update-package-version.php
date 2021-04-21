<?php
$e = file_get_contents( $argv[1] . '/php_timezonedb.h' );

/* Grab version info */
$version_info = file( 'version-info.txt' );
$tzversion = trim( $version_info[0] );
$version = trim( $version_info[1] );

/* Use preg_replace to update version in package.xml */
$f = preg_replace( '@"20[0-2][0-9].*"@', "\"{$version}\"", $e );

file_put_contents( $argv[1] . '/php_timezonedb.h', $f );
?>
