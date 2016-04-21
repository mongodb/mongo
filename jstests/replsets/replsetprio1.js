// should check that election happens in priority order

(function() {
    "use strict";
    var replTest = new ReplSetTest({name: 'testSet', nodes: 3});
    var nodenames = replTest.nodeList();

    var nodes = replTest.startSet();
    replTest.initiate({
        "_id": "testSet",
        "members": [
            {"_id": 0, "host": nodenames[0], "priority": 1},
            {"_id": 1, "host": nodenames[1], "priority": 2},
            {"_id": 2, "host": nodenames[2], "priority": 3}
        ]
    });

    // 2 should be master (give this a while to happen, as other nodes might first be elected)
    replTest.waitForState(nodes[2], ReplSetTest.State.PRIMARY, 120000);
    // wait for 1 to not appear to be master (we are about to make it master and need a clean slate
    // here)
    replTest.waitForState(nodes[1], ReplSetTest.State.SECONDARY, 60000);

    // Wait for election oplog entry to be replicated, to ensure 0 will vote for 1 after stopping 2.
    replTest.awaitReplication();

    // kill 2, 1 should take over
    replTest.stop(2);

    // 1 should eventually be master
    replTest.waitForState(nodes[1], ReplSetTest.State.PRIMARY, 60000);

    // do some writes on 1
    var master = replTest.getPrimary();
    for (var i = 0; i < 1000; i++) {
        assert.writeOK(master.getDB("foo").bar.insert({i: i}, {writeConcern: {w: 'majority'}}));
    }

    for (i = 0; i < 1000; i++) {
        assert.writeOK(master.getDB("bar").baz.insert({i: i}, {writeConcern: {w: 'majority'}}));
    }

    // bring 2 back up, 2 should wait until caught up and then become master
    replTest.restart(2);
    replTest.waitForState(nodes[2], ReplSetTest.State.PRIMARY, 60000);

    // make sure nothing was rolled back
    master = replTest.getPrimary();
    for (i = 0; i < 1000; i++) {
        assert(master.getDB("foo").bar.findOne({i: i}) != null, 'checking ' + i);
        assert(master.getDB("bar").baz.findOne({i: i}) != null, 'checking ' + i);
    }
}());
