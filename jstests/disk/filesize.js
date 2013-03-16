// test for SERVER-7430: Warning about smallfiles should include filename
var port = allocatePorts( 1 )[ 0 ];
var baseName = "filesize";

// Start mongod with --smallfiles
var m = startMongod(
    "--port", port, "--dbpath", "/data/db/" + baseName, "--nohttpinterface",
    "--bind_ip", "127.0.0.1" , "--nojournal" , "--smallfiles" );

var db = m.getDB( baseName );

// Skip on 32 bits, since 32-bit servers don't warn about small files
if (db.serverBuildInfo().bits == 32) {
    print("Skip on 32-bit");
} else {
    // Restart mongod without --smallFiles
    stopMongod( port );
    m = startMongodNoReset(
        "--port", port, "--dbpath", "/data/db/" + baseName,
        "--nohttpinterface", "--bind_ip", "127.0.0.1" , "--nojournal" );

    db = m.getDB( baseName );
    var log = db.adminCommand( { getLog : "global" } ).log

    // Find log message like:
    // "openExisting file size 16777216 but cmdLine.smallfiles=false: /data/db/filesize/local.0"
    var found = false, logline = '';
    for ( i=log.length - 1; i>= 0; i-- ) {
        logline = log[i];
        if ( logline.indexOf( "openExisting file" ) >= 0
            && logline.indexOf( "local.0" ) >= 0 ) {
            found = true;
            break;
        }
    }

    assert( found );
}
