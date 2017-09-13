// test that the resync command works with replica sets and that one does not need to manually
// force a replica set resync by deleting all datafiles
// Also tests that you can do this from a node that is "too stale"
//
// This test requires persistence in order for a restarted node with a stale oplog to stay in the
// RECOVERING state. A restarted node with an ephemeral storage engine will not have an oplog upon
// restart, so will immediately resync.
// @tags: [requires_persistence]
(function() {
    "use strict";

    var replTest = new ReplSetTest({
        name: 'resync',
        nodes: 3,
        oplogSize: 1,
        // At the end of this test we call resync on a node that may have blacklisted the only other
        // data bearing node.  We need to ensure that the resync attempt will keep looking for a
        // sync source for at least 60 seconds, until the blacklist period ends.  Since we sleep 1
        // second in between each attempt to find a sync source, setting the number of attempts to
        // find a sync source to larger than 60 should ensure that the resync attempt is able to
        // succeed.
        nodeOptions: {setParameter: "numInitialSyncConnectAttempts=90"}
    });
    var nodes = replTest.nodeList();

    var conns = replTest.startSet();
    var r = replTest.initiate({
        "_id": "resync",
        "members": [
            {"_id": 0, "host": nodes[0], priority: 1},
            {"_id": 1, "host": nodes[1], priority: 0},
            {"_id": 2, "host": nodes[2], arbiterOnly: true}
        ]
    });

    var a_conn = conns[0];
    // Make sure we have a master, and it is conns[0]
    replTest.waitForState(a_conn, ReplSetTest.State.PRIMARY);
    var b_conn = conns[1];
    a_conn.setSlaveOk();
    b_conn.setSlaveOk();
    var A = a_conn.getDB("test");
    var B = b_conn.getDB("test");
    var AID = replTest.getNodeId(a_conn);
    var BID = replTest.getNodeId(b_conn);

    // create an oplog entry with an insert
    assert.writeOK(A.foo.insert({x: 1}, {writeConcern: {w: 2, wtimeout: 60000}}));
    assert.eq(B.foo.findOne().x, 1);

    // run resync and wait for it to happen
    assert.commandWorked(b_conn.getDB("admin").runCommand({resync: 1}));
    replTest.awaitReplication();
    replTest.awaitSecondaryNodes();

    assert.eq(B.foo.findOne().x, 1);
    replTest.stop(BID);

    function hasCycled() {
        var oplog = a_conn.getDB("local").oplog.rs;
        try {
            // Collection scan to determine if the oplog entry from the first insert has been
            // deleted yet.
            return oplog.find({"o.x": 1}).sort({$natural: 1}).limit(10).itcount() == 0;
        } catch (except) {
            // An error is expected in the case that capped deletions blow away the position of the
            // collection scan during a yield. In this case, we just try again.
            var errorRegex = /CappedPositionLost/;
            assert(errorRegex.test(except.message));
            return hasCycled();
        }
    }

    jsTestLog("Rolling over oplog");

    // Make sure the oplog has rolled over on the primary and secondary that is up,
    // so when we bring up the other replica it is "too stale"
    for (var cycleNumber = 0; cycleNumber < 10; cycleNumber++) {
        // insert enough to cycle oplog
        var bulk = A.foo.initializeUnorderedBulkOp();
        for (var i = 2; i < 10000; i++) {
            bulk.insert({x: i});
        }

        // wait for secondary to also have its oplog cycle
        assert.writeOK(bulk.execute({w: 1, wtimeout: 60000}));

        if (hasCycled())
            break;
    }

    assert(hasCycled());

    jsTestLog("Restarting node B");
    // bring node B and it will enter recovery mode because its newest oplog entry is too old
    replTest.restart(BID);

    jsTestLog("Waiting for node B to to into RECOVERING");
    // check that it is in recovery mode
    assert.soon(function() {
        try {
            var result = b_conn.getDB("admin").runCommand({replSetGetStatus: 1});
            return (result.members[1].stateStr === "RECOVERING");
        } catch (e) {
            print(e);
        }
    }, "node didn't enter RECOVERING state");

    jsTestLog("Resync node B");
    // run resync and wait for it to happen
    assert.commandWorked(b_conn.getDB("admin").runCommand({resync: 1}));
    replTest.awaitReplication();
    replTest.awaitSecondaryNodes();
    assert.eq(B.foo.findOne().x, 1);

    replTest.stopSet(15);
    jsTest.log("success");
})();
