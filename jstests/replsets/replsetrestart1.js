doTest = function( signal ) {

    // Make sure that we can restart a replica set completely

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

    // Now we're going to shut down all nodes
    mId  = replTest.getNodeId( master );
    s1Id = replTest.getNodeId( replTest.liveNodes.slaves[0] );
    s2Id = replTest.getNodeId( replTest.liveNodes.slaves[1] );

    replTest.stop( s1Id );
    replTest.stop( s2Id );
    
    assert.soon(function() {
            var status = master.getDB("admin").runCommand({replSetGetStatus: 1});
            return status.members[1].state == 8 && status.members[2].state == 8;
        });

    
    replTest.stop( mId );

    // Now let's restart these nodes
    replTest.restart( mId );
    replTest.restart( s1Id );
    replTest.restart( s2Id );

    // Make sure that a new master comes up
    master = replTest.getMaster();
    slaves = replTest.liveNodes.slaves;

    assert.soon(function() {
            var status = master.getDB("admin").runCommand({replSetGetStatus: 1});
            return status.members[1].state != 8 && status.members[2].state != 8;
        });
    
    // Do a status check on each node
    // Master should be set to 1 (primary)
    assert.soon(function() {
        stat = master.getDB("admin").runCommand({replSetGetStatus: 1});
        return stat.myState == 1;
    }, "checking master", 3 * 60 * 1000, 1000 );

    // Slaves to be set to 2 (secondary)
    assert.soon(function() {
        stat = slaves[0].getDB("admin").runCommand({replSetGetStatus: 1});
        return stat.myState == 2;
    }, "checking slave 0", 3 * 60 * 1000, 1000 );

    assert.soon(function() {
        stat = slaves[1].getDB("admin").runCommand({replSetGetStatus: 1});
        return stat.myState == 2;
    }, "checking slave 1", 3 * 60 * 1000, 1000 );
}

doTest( 15 );
