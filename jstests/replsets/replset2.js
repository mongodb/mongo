
doTest = function( signal ) {

    // Test replication with getLastError

    // Replica set testing API
    // Create a new replica set test. Specify set name and the number of nodes you want.
    var replTest = new ReplSetTest( {name: 'testSet', nodes: 3} );

    // call startSet() to start each mongod in the replica set
    // this returns a list of nodes
    var nodes = replTest.startSet();

    // Call initiate() to send the replSetInitiate command
    // This will wait for initiation
    replTest.initiate();

    // Call getMaster to return a reference to the node that's been
    // elected master.
    var master = replTest.getMaster();

    // Wait for replication to a single node
    master.getDB("foo").bar.insert({n: 1});
    var result = master.getDB("foo").runCommand({getlasterror: {w: 1, wtimeout: 20000}});
    assert( result['ok'] == 1, "getLastError with w=2 failed");

    // Wait for replication two two nodes
    master.getDB("foo").bar.insert({n: 2});
    var result = master.getDB("foo").runCommand({getlasterror: {w: 2, wtimeout: 20000}});
    assert( result['ok'] == 1, "getLastError with w=2 failed");

    // Wait for replication to three nodes
    master.getDB("foo").bar.insert({n: 3});
    var result = master.getDB("foo").runCommand({getlasterror: {w: 3, wtimeout: 20000}});
    assert( result['ok'] == 1, "getLastError with w=2 failed");

    replTest.stopSet( signal );
}

doTest( 15 );
