// Election when master fails and remaining nodes are an arbiter and a slave.

(function() {
    "use strict";

    var replTest = new ReplSetTest( {name: 'unicomplex', nodes: 3} );
    var nodes = replTest.nodeList();

    var conns = replTest.startSet();
    var r = replTest.initiate({"_id" : "unicomplex",
                               "members" : [
                                   {"_id" : 0, "host" : nodes[0], "priority" : 2},
                                   {"_id" : 1, "host" : nodes[1], "arbiterOnly" : true, "votes": 1},
                                   {"_id" : 2, "host" : nodes[2]}
                                ]});

    // Make sure we have a master
    var master = replTest.getMaster();
    assert.eq(master, conns[0], "wrong node became master");

    // Make sure we have an arbiter
    assert.soon(function() {
        var res = conns[1].getDB("admin").runCommand({replSetGetStatus: 1});
        printjson(res);
        return res.myState === 7;
    }, "Aribiter failed to initialize.");

    var result = conns[1].getDB("admin").runCommand({isMaster : 1});
    assert(result.arbiterOnly);
    assert(!result.passive);

    // Wait for initial replication
    master.getDB("foo").foo.insert({a: "foo"});
    replTest.awaitReplication();

    // Now kill the original master
    var mId = replTest.getNodeId(master);
    replTest.stop(mId);

    // And make sure that the slave is promoted
    var new_master = replTest.getMaster();

    var newMasterId = replTest.getNodeId(new_master);
    assert.eq(newMasterId, 2, "Secondary wasn't promoted to new primary");

    replTest.stopSet(15);
}());
