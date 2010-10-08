// running ops should be killed
// dropped collection should be ok after restart

port = allocatePorts( 1 )[ 0 ]

var baseName = "jstests_disk_killall";

var m = startMongod( "--port", port, "--dbpath", "/data/db/" + baseName );

m.getDB( "test" ).getCollection( baseName ).save( {} );
m.getDB( "test" ).getLastError();

s1 = startParallelShell( "db." + baseName + ".count( { $where: function() { while( 1 ) { ; } } } )", port );
sleep( 1000 );

s2 = startParallelShell( "db." + baseName + ".drop()", port );
sleep( 1000 );

assert.eq( 12, stopMongod( port ) ); // 12 == interrupt exit code

s1();
s2();

var m = startMongoProgram( "mongod", "--port", port, "--dbpath", "/data/db/" + baseName );

m.getDB( "test" ).getCollection( baseName ).stats();
m.getDB( "test" ).getCollection( baseName ).drop();

stopMongod( port );
