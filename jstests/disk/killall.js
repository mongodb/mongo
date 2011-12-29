// running ops should be killed
// dropped collection should be ok after restart

//if ( typeof _threadInject == "undefined" ) { // don't run in v8 mode - SERVER-2076

port = allocatePorts( 1 )[ 0 ]

var baseName = "jstests_disk_killall";

var m = startMongod( "--port", port, "--dbpath", "/data/db/" + baseName, "--nohttpinterface" );

m.getDB( "test" ).getCollection( baseName ).save( {} );
m.getDB( "test" ).getLastError();

s1 = startParallelShell( "db." + baseName + ".count( { $where: function() { while( 1 ) { ; } } } )", port );
sleep( 1000 );

s2 = startParallelShell( "db." + baseName + ".drop()", port );
sleep( 1000 );

/**
 * 12 == mongod's exit code on interrupt (eg standard kill)
 * stopMongod sends a standard kill signal to mongod, then waits for mongod to stop.  If mongod doesn't stop
 * in a reasonable amount of time, stopMongod sends kill -9 and in that case will not return 12.  We're checking
 * in this assert that mongod will stop quickly even while evaling an infinite loop in server side js.
 *
 * 14 is sometimes returned instead due to SERVER-2184
 */
exitCode = stopMongod( port );
assert( exitCode == 12 || exitCode == 14 || (_isWindows() && (exitCode == 0)), "got unexpected exitCode: " + exitCode );

s1();
s2();

var m = startMongoProgram( "mongod", "--port", port, "--dbpath", "/data/db/" + baseName );

m.getDB( "test" ).getCollection( baseName ).stats();
m.getDB( "test" ).getCollection( baseName ).drop();

stopMongod( port );

//}
