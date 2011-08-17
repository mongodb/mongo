// Election when master fails and remaining nodes are an arbiter and a slave.

doTest = function( signal ) {

    var replTest = new ReplSetTest( {name: 'unicomplex', nodes: 3} );
    var nodes = replTest.nodeList();

    print(tojson(nodes));

    var conns = replTest.startSet();
    var r = replTest.initiate({"_id" : "unicomplex",
                "members" : [
                    {"_id" : 0, "host" : nodes[0] },
                    {"_id" : 1, "host" : nodes[1], "arbiterOnly" : true, "votes": 1, "priority" : 0},
                    {"_id" : 2, "host" : nodes[2] }]});

    // Make sure we have a master
    var master = replTest.getMaster();

    // Make sure we have an arbiter
    assert.soon(function() {
        res = conns[1].getDB("admin").runCommand({replSetGetStatus: 1});
        printjson(res);
        return res.myState == 7;
    }, "Aribiter failed to initialize.");

    var result = conns[1].getDB("admin").runCommand({isMaster : 1});
    assert(result.arbiterOnly);
    assert(!result.passive);

    // Wait for initial replication
    master.getDB("foo").foo.insert({a: "foo"});
    replTest.awaitReplication();

    assert( ! conns[1].getDB( "admin" ).runCommand( "ismaster" ).secondary , "arbiter shouldn't be secondary" )

    // Now kill the original master
    mId = replTest.getNodeId( master );
    replTest.stop( mId );

    // And make sure that the slave is promoted
    new_master = replTest.getMaster();

    newMasterId = replTest.getNodeId( new_master );
    assert( newMasterId == 2, "Slave wasn't promoted to new master");

    replTest.stopSet( signal );
}

doTest( 15 );
