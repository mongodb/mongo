// SERVER-4560 test

port = allocatePorts( 1 )[ 0 ]
var baseName = "jstests_disk_newcollection1";
var m = startMongod( "--noprealloc", "--smallfiles", "--port", port, "--dbpath", "/data/db/" + baseName );

db = m.getDB( "test" );

db.dropDatabase();

var colls = db.getCollectionNames();

assert ( colls.length==0 );

db.createCollection("broken", {capped: true, size: -1});

var new_colls = db.getCollectionNames();

print (new_colls);
assert( new_colls.length==0 );
