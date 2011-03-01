// Alocate collection forcing just a small size remainder in 2nd extent

port = allocatePorts( 1 )[ 0 ]
var baseName = "jstests_disk_newcollection2";
var m = startMongod( "--noprealloc", "--smallfiles", "--port", port, "--dbpath", "/data/db/" + baseName );
db = m.getDB( "test" );

db.createCollection( baseName, {size:0x1FFC0000-0x10-8192} );
var v = db[ baseName ].validate();
printjson( v );
assert( v.valid );
