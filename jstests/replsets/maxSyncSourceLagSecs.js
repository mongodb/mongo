// Test that setting maxSyncSourceLagSecs causes the set to change sync target
//
// This test requires the fsync command to ensure members experience a delay.
// @tags: [requires_fsync]
(function() {
    "use strict";
    load("jstests/replsets/rslib.js");

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

    // Disable maxSyncSourceLagSecs behavior until we've established the spanning tree we want.
    replTest.nodes.forEach(function(node) {
        assert.commandWorked(node.getDB('admin').runCommand(
            {configureFailPoint: 'disableMaxSyncSourceLagSecs', mode: 'alwaysOn'}));
    });

    var master = replTest.getPrimary();
    var slaves = replTest.liveNodes.slaves;
    syncFrom(slaves[0], master, replTest);
    syncFrom(slaves[1], master, replTest);
    master.getDB("foo").bar.save({a: 1});
    replTest.awaitReplication();

    jsTestLog("Setting sync target of slave 2 to slave 1");
    syncFrom(slaves[1], slaves[0], replTest);
    printjson(replTest.status());

    // need to put at least maxSyncSourceLagSecs b/w first op and subsequent ops
    // so that the shouldChangeSyncSource logic goes into effect
    sleep(4000);

    // Re-enable maxSyncSourceLagSecs behavior now that we have the spanning tree we want and are
    // ready to test that behavior.
    replTest.nodes.forEach(function(node) {
        assert.commandWorked(node.getDB('admin').runCommand(
            {configureFailPoint: 'disableMaxSyncSourceLagSecs', mode: 'off'}));
    });

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
