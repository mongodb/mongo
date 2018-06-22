// Test to ensure that catchup takeover runs even if it isn't the highest
// priority node and that once the high priority node is caught up,
// it becomes primary again.

// 3-node replica set with one high priority node.
// Start replica set. Make node 0 primary and stop the replication
// for the high priority node as well as isolate it. Have the
// primary write something so node 2 is more than 2 seconds behind.
// Write something else to ensure the third node is also lagged.
// Reconnect the high priority node to the other nodes and make
// the lagged node (node 1) the next primary.
// Confirm that the most up-to-date node becomes primary.
// Let the highest priority node catchup and then confirm
// that it becomes primary.

(function() {
    'use strict';

    load('jstests/replsets/rslib.js');

    var name = 'catchup_takeover_one_high_priority';
    var replSet = new ReplSetTest({name: name, nodes: 3, useBridge: true});

    var nodenames = replSet.nodeList();
    var nodes = replSet.startSet();
    replSet.initiateWithAnyNodeAsPrimary({
        "_id": name,
        "members": [
            {"_id": 0, "host": nodenames[0]},
            {"_id": 1, "host": nodenames[1]},
            {"_id": 2, "host": nodenames[2], "priority": 2}
        ]
    });

    // Wait until node 2 becomes primary.
    replSet.waitForState(2, ReplSetTest.State.PRIMARY, replSet.kDefaultTimeoutMS);
    jsTestLog('node 2 is now primary');

    replSet.awaitReplication();

    // Stop replication and disconnect node 2 so that it cannot do a priority takeover.
    stopServerReplication(nodes[2]);
    nodes[2].disconnect(nodes[1]);
    nodes[2].disconnect(nodes[0]);

    // Ensure that node 0 becomes primary.
    assert.commandWorked(nodes[0].adminCommand({replSetStepUp: 1}));
    replSet.awaitNodesAgreeOnPrimary(replSet.kDefaultTimeoutMS, nodes.slice(0, 2));
    assert.eq(ReplSetTest.State.PRIMARY,
              assert.commandWorked(nodes[0].adminCommand('replSetGetStatus')).myState,
              nodes[0].host + " was not primary after step-up");
    jsTestLog('node 0 is now primary');

    // Sleep for a few seconds to ensure that node 2's optime is more than 2 seconds behind.
    // This will ensure it can't do a priority takeover until it catches up.
    sleep(3000);

    var primary = replSet.getPrimary();
    var writeConcern = {writeConcern: {w: 2, wtimeout: replSet.kDefaultTimeoutMS}};
    assert.writeOK(primary.getDB(name).bar.insert({y: 100}, writeConcern));

    // Write something so that node 0 is ahead of node 1.
    stopServerReplication(nodes[1]);
    writeConcern = {writeConcern: {w: 1, wtimeout: replSet.kDefaultTimeoutMS}};
    assert.writeOK(primary.getDB(name).bar.insert({x: 100}, writeConcern));

    nodes[2].reconnect(nodes[0]);
    nodes[2].reconnect(nodes[1]);

    // Step up a lagged node.
    assert.commandWorked(nodes[1].adminCommand({replSetStepUp: 1}));
    replSet.awaitNodesAgreeOnPrimary(replSet.kDefaultTimeoutMS, nodes);
    assert.eq(ReplSetTest.State.PRIMARY,
              assert.commandWorked(nodes[1].adminCommand('replSetGetStatus')).myState,
              nodes[1].host + " was not primary after step-up");
    jsTestLog('node 1 is now primary, but cannot accept writes');

    // Confirm that the most up-to-date node becomes primary
    // after the default catchup delay.
    replSet.waitForState(0, ReplSetTest.State.PRIMARY, 60 * 1000);
    jsTestLog('node 0 performed catchup takeover and is now primary');

    // Wait until the old primary steps down.
    replSet.awaitNodesAgreeOnPrimary();

    // Let the nodes catchup.
    restartServerReplication(nodes[1]);
    restartServerReplication(nodes[2]);

    // Confirm that the highest priority node becomes primary
    // after catching up.
    replSet.waitForState(2, ReplSetTest.State.PRIMARY, 30 * 1000);
    jsTestLog('node 2 performed priority takeover and is now primary');

    // Wait until the old primary steps down so the connections won't be closed during stopSet().
    replSet.waitForState(0, ReplSetTest.State.SECONDARY, replSet.kDefaultTimeoutMS);

    replSet.stopSet();
})();
