// test for SERVER-7430: Warning about smallfiles should include filename

var port = allocatePorts( 1 )[ 0 ];
var baseName = "filesize";

// Start mongod with --smallfiles
var m = startMongod(
    "--port", port, "--dbpath", "/data/db/" + baseName, "--nohttpinterface",
    "--bind_ip", "127.0.0.1" , "--nojournal" , "--smallfiles" );

var db = m.getDB( baseName );
db.collection.insert( { x : 1 } );

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
        && logline.indexOf( baseName + ".0" ) >= 0 ) {
        found = true;
        break;
    }
}

assert( found );
