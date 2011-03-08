// Test the (legacy) distributed lock with significant time skewing.
// Negative test does not always pass, so turned off, but useful to have around.

test = new SyncCCTest( "sync5-", { logpath : "/dev/null" } )

var commandConn = startMongodTest( 30000 + 4, "syncCommander", false, { logpath : "/dev/null" } )
// { logpath : "/data/db/syncCommander/mongod.log" } );

for ( var i = 0; i < 10; i++ ) {

	command = { _testDistLockWithSkew : 1 }
	command["lockName"] = "TimeSkewTest_lock_" + i;
	command["host"] = test.url
	command["seed"] = i
	command["numThreads"] = ( i % 50 ) + 1
	command["wait"] = 1000

	// Use the legacy logic with 15 mins per lock.
	command["takeoverMins"] = 15
	// Amount of time to hold the lock
	command["threadWait"] = ( ( i + 1 ) * 10 ) % 200

	// Amount of total clock skew possible between locking threads (processes)
	// Must be much less than the takeover minutes
	// TODO:  Put divisor back to 2, if possible
	command["skewRange"] = ( command["takeoverMins"] / 5 ) * 60 * 1000

	result = commandConn.getDB( "admin" ).runCommand( command )
	printjson( result )
	printjson( command )
	assert( result.ok, "Skewed threads did not increment correctly." );

}

if ( false ) {

	// Test that the distributed lock fails with more skew.

	var badTakeover = false;
	for ( var i = 0; i < 10 && !badTakeover; i++ ) {

		command = { _testDistLockWithSkew : 1 }
		command["host"] = test.url
		command["seed"] = i
		command["numThreads"] = ( i % 50 ) + 1
		command["wait"] = 1000

		// Use the legacy logic with 15 mins per lock.
		command["takeoverMins"] = 15

		// Amount of time to hold the lock
		command["threadWait"] = ( ( i + 1 ) * 10 ) % 200

		// Amount of total clock skew possible between locking threads
		// (processes)
		// Must be much less than the takeover minutes
		command["skewRange"] = ( command["takeoverMins"] * 2 ) * 60 * 1000
		command["localTimeout"] = true

		result = commandConn.getDB( "admin" ).runCommand( command )
		printjson( result )
		printjson( command )

		badTakeover = !result.ok;
	}
	assert( badTakeover, "Skewed threads did not interfere with one another." );

}

test.stop();
