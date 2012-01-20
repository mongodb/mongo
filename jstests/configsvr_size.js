// SERVER-4297  Test for appropriate data file size on config server
t = db.configsvr_size;
port = allocatePorts( 1 )[ 0 ];
baseName = "jstests_configsvr_size";
dbPath = "/data/db/" + baseName;

// start the server
var m = startMongod( "--port", port, "--dbpath", dbPath, "--configsvr" );
var db = m.getDB( "config" );
var found = false;

// insert an entry to the collection
db.test.insert( { test: 1 } );
db.runCommand( { getLastError: 1, fsync:1 } );

var files = listFiles(dbPath);
for (var f in files) {
    if ( files[f].name != dbPath + "/config.0" )
        continue;

    assert( files[f].size == 16777216, "Invalid data file size for config server.  Expected 16777216, got " + files[f].size );
    found = true;
}

assert(found, "Could not find test db file: " + dbPath + "/config.0");