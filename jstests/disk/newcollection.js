// SERVER-594 test

port = allocatePorts( 1 )[ 0 ]
var baseName = "jstests_disk_newcollection";
var m = startMongod( "--noprealloc", "--smallfiles", "--port", port, "--dbpath", "/data/db/" + baseName );
db = m.getDB( "test" );

db.createCollection( baseName, {size:15.9*1024*1024} );
db.baseName.drop();

size = m.getDBs().totalSize;
db.baseName.save( {} );
assert.eq( size, m.getDBs().totalSize );
