doTest = function( signal ) {

    // Test basic replica set functionality.
    // -- Replication
    // -- Failover

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

    // Calling getMaster also makes available the liveNodes structure,
    // which looks like this:
    // liveNodes = {master: masterNode,
    //              slaves: [slave1, slave2]
    //             }
    printjson(replTest.liveNodes);

    // Here's how you save something to master
    master.getDB("foo").foo.save({a: 1000});

    // This method will check the oplogs of the master
    // and slaves in the set and wait until the change has replicated.
    replTest.awaitReplication();

    // Here's how to stop the master node
    var master_id = replTest.getNodeId( master );
    replTest.stop( master_id );

    // Now let's see who the new master is:
    var new_master = replTest.getMaster();

    // Is the new master the same as the old master?
    var new_master_id = replTest.getNodeId( new_master );

    assert( master_id != new_master_id, "Old master shouldn't be equal to new master." );

    // Here's how to restart a node:
    replTest.start( master_id, {}, true );


    // Shut down the set and finish the test.
    replTest.stopSet( signal );
}

doTest( 15 );
