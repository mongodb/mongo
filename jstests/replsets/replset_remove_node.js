(function() {

    // Make sure that we can manually shutdown and remove a
    // secondary from the configuration.

    var replTest = new ReplSetTest( {name: 'testSet', nodes: 3} );

    // call startSet() to start each mongod in the replica set
    // this returns a list of nodes
    var nodes = replTest.startSet();

    // Call initiate() to send the replSetInitiate command
    // This will wait for initiation
    var name = replTest.nodeList();
    replTest.initiate({"_id" : "testSet",
                       "members" : [
                           // make sure 0 becomes primary so we don't try to remove the
                           // primary below
                           {"_id" : 0, "host" : name[0], priority:2},
                           {"_id" : 1, "host" : name[1]},
                           {"_id" : 2, "host" : name[2]}]});

    // Call getMaster to return a reference to the node that's been
    // elected master.
    var master = replTest.getMaster();

    // Reconfigure the set, removing the unwanted node
    slaveId = replTest.getNodeId( replTest.liveNodes.slaves[0] );

    // Shut down the unwanted node
    replTest.stop( slaveId );
    // Note: this will cause the PRIMARY to step down, which causes it to close all
    // connections, including the one we're using from the shell to drive this test.
    // The shell will attempt to reconnect once.


    // Remove that node from the configuration
    replTest.remove( slaveId );

    // Now, re-initiate
    var c = master.getDB("local")['system.replset'].findOne();
    var config  = replTest.getReplSetConfig();
    config.version = c.version + 1;
    config.members = [ { "_id" : 0, "host" : replTest.host + ":31000" },
                       { "_id" : 2, "host" : replTest.host + ":31002" } ];

    try {
        // Note that this will cause the shell's connection to the primary to be disconnected again
        replTest.initiate( config , 'replSetReconfig' );
    }
    catch(e) {
        print(e);
    }

    // Make sure that a new master comes up
    master = replTest.getMaster();
    slaves = replTest.liveNodes.slaves;

    // Trigger a reconnect from the shell again, to be sure we're reconnected.
    try {
        master.getDB("local").system.replset.findOne();
    }
    catch (e) {
        print (e);
    }

    // Do a status check on each node
    // Master should be set to 1 (primary)
    assert.soon(function() {
        var stat = master.getDB("admin").runCommand({replSetGetStatus: 1});
        printjson( stat );
        return stat.myState == 1;
    }, "Master failed to come up as master.", 60000);

    // Slaves to be set to 2 (secondary)
    assert.soon(function() {
        var stat = slaves[0].getDB("admin").runCommand({replSetGetStatus: 1});
        return stat.myState == 2;
    }, "Slave failed to come up as slave.", 60000);

    assert.soon(function() {
        var stat = slaves[0].getDB("admin").runCommand({replSetGetStatus: 1});
        return stat.members.length == 2;
    }, "Wrong number of members", 60000);
}());
