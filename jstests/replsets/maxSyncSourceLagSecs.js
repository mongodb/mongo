// Test that setting maxSyncSourceLagSecs causes the set to change sync target
//
// This test requires the fsync command to ensure members experience a delay.
// @tags: [requires_fsync]
(function() {
    "use strict";
    var name = "maxSyncSourceLagSecs";
    var replTest = new ReplSetTest({
        name: name,
        nodes: 3,
        oplogSize: 5,
        nodeOptions: {setParameter: "maxSyncSourceLagSecs=3"}
    });
    var nodes = replTest.nodeList();
    replTest.startSet();
    replTest.initiate({
        "_id": name,
        "members": [
            {"_id": 0, "host": nodes[0], priority: 3},
            {"_id": 1, "host": nodes[1], priority: 0},
            {"_id": 2, "host": nodes[2], priority: 0}
        ],
    });
    replTest.awaitNodesAgreeOnPrimary();

    var master = replTest.getPrimary();
    var slaves = replTest.liveNodes.slaves;
    assert.commandWorked(slaves[0].getDB("admin").runCommand({replSetSyncFrom: master.name}));
    assert.commandWorked(slaves[1].getDB("admin").runCommand({replSetSyncFrom: master.name}));
    master.getDB("foo").bar.save({a: 1});
    replTest.awaitReplication();

    // need to put at least maxSyncSourceLagSecs b/w first op and subsequent ops
    // so that the shouldChangeSyncSource logic goes into effect
    sleep(4000);

    jsTestLog("Setting sync target of slave 2 to slave 1");
    assert.soon(function() {
        // We do a write each time and have this in a try...catch block due to the fallout of
        // SERVER-24114. If that timeout occurs, then we search for another sync source, however we
        // will not find one unless more writes have come in. Additionally, it is possible that
        // slaves[1] will switch to sync from slaves[0] after slaves[1] replicates a write from
        // the primary but before slaves[0] replicates it. slaves[1] will then have to roll back
        // which would cause a network error.
        try {
            slaves[1].getDB("admin").runCommand({replSetSyncFrom: slaves[0].name});
            var res = slaves[1].getDB("admin").runCommand({"replSetGetStatus": 1});
            master.getDB("foo").bar.insert({a: 1});
            return res.syncingTo === slaves[0].name;
        } catch (e) {
            print("Exception in assert.soon, retrying: " + e);
            return false;
        }
    }, "sync target not changed to other slave", 100 * 1000, 2 * 1000);
    printjson(replTest.status());

    jsTestLog("Lock slave 1 and add some docs. Force sync target for slave 2 to change to primary");
    assert.commandWorked(slaves[0].getDB("admin").runCommand({fsync: 1, lock: 1}));

    assert.soon(function() {
        master.getDB("foo").bar.insert({a: 2});
        var res = slaves[1].getDB("admin").runCommand({"replSetGetStatus": 1});
        return res.syncingTo === master.name;
    }, "sync target not changed back to primary", 100 * 1000, 2 * 1000);
    printjson(replTest.status());

    assert.soon(function() {
        return (slaves[1].getDB("foo").bar.count({a: 1}) > 0 &&
                slaves[1].getDB("foo").bar.count({a: 2}) > 0);
    }, "slave should have caught up after syncing to primary.");

    assert.commandWorked(slaves[0].getDB("admin").fsyncUnlock());
    replTest.stopSet();
}());
