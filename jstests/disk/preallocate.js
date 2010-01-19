var baseName = "jstests_preallocate";

vsize = function() {
    return m.getDB( "admin" ).runCommand( "serverStatus" ).mem.virtual;
}

var m = startMongod( "--port", "27018", "--dbpath", "/data/db/" + baseName );

m.getDB( baseName ).createCollection( baseName + "1" );

vs = vsize();

stopMongod( 27018 );

var m = startMongoProgram( "mongod", "--port", "27018", "--dbpath", "/data/db/" + baseName );

m.getDB( baseName ).createCollection( baseName + "2" );

assert.eq( vs, vsize() );
