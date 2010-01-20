port = allocatePorts( 1 )[ 0 ]

var baseName = "jstests_preallocate";

vsize = function() {
    return m.getDB( "admin" ).runCommand( "serverStatus" ).mem.virtual;
}

var m = startMongod( "--port", port, "--dbpath", "/data/db/" + baseName );

m.getDB( baseName ).createCollection( baseName + "1" );

vs = vsize();

stopMongod( port );

var m = startMongoProgram( "mongod", "--port", port, "--dbpath", "/data/db/" + baseName );

m.getDB( baseName ).createCollection( baseName + "2" );

assert.eq( vs, vsize() );
