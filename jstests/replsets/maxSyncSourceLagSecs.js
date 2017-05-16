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
        nodes: [
            {rsConfig: {priority: 3}},
            {rsConfig: {priority: 0}},
            {rsConfig: {priority: 0}, setParameter: 'maxSyncSourceLagSecs=3'},
        ],
        oplogSize: 5,
    });
    var nodes = replTest.nodeList();
    replTest.startSet();
    replTest.initiate();
    replTest.awaitNodesAgreeOnPrimary();

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
