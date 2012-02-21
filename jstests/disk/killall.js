/*
 * Verify that killing an instance of mongod while it is in a long running computation or infinite
 * loop still leads to clean shutdown, and that said shutdown is prompt.
 *
 * For our purposes, "prompt" is defined as "before stopMongod() decides to send a SIGKILL" and also
 * "before mongod starts executing the next pending operation after the currently executing one."
 *
 * This is also a regression test for SERVER-1818.  By queuing up a collection drop behind an
 * infinitely looping operation, we ensure that killall interrupts the drop command (drop is
 * normally a short lived operation and is otherwise difficult to kill deterministically).  The
 * subsequent drop() on restart would historically assert if the data integrity issue caused by
 * SERVER-1818 occured.
 */

port = allocatePorts( 1 )[ 0 ]

var baseName = "jstests_disk_killall";
var dbpath = "/data/db/" + baseName;

var mongod = startMongod( "--port", port, "--dbpath", dbpath, "--nohttpinterface" );
var db = mongod.getDB( "test" );
var collection = db.getCollection( baseName );

collection.save( {} );
assert( ! db.getLastError() );

s1 = startParallelShell( "db." + baseName + ".count( { $where: function() { while( 1 ) { ; } } } )", port );
// HACK(schwerin): startParallelShell's return value should allow you to block until the command has
// started, for some definition of started.
sleep( 1000 );

s2 = startParallelShell( "db." + baseName + ".drop()", port );
sleep( 1000 );  // HACK(schwerin): See above.

/**
 * 0 == mongod's exit code on Windows, or when it receives TERM, HUP or INT signals.  On UNIX
 * variants, stopMongod sends a TERM signal to mongod, then waits for mongod to stop.  If mongod
 * doesn't stop in a reasonable amount of time, stopMongod sends a KILL signal, in which case mongod
 * will not exit cleanly.  We're checking in this assert that mongod will stop quickly even while
 * evaling an infinite loop in server side js.
 *
 * NOTE: 14 is sometimes returned instead due to SERVER-2652.
 */
var exitCode = stopMongod( port );
assert( exitCode in [0, 14], "got unexpected exitCode: " + exitCode );

s1();
s2();

mongod = startMongoProgram( "mongod", "--port", port, "--dbpath", dbpath );
db = mongod.getDB( "test" );
collection = db.getCollection( baseName );

assert( collection.stats().ok );
assert( collection.drop() );

stopMongod( port );
