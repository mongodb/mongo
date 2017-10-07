// Test to ensure that a catchup takeover happens when the primary is lagged.
// Make sure that when two nodes are more caught up than the primary,
// the most up-to-date node becomes the primary.

// 5-node replica set
// Start replica set. Ensure that node 0 becomes primary.
// Stop the replication for some nodes and have the primary write something.
// Stop replication for an up-to-date node and have the primary write something.
// Now the primary is most-up-to-date and another node is more up-to-date than others.
// Make a lagged node the next primary.
// Confirm that the most up-to-date node becomes primary.

(function() {
    'use strict';

    load('jstests/replsets/rslib.js');

    var name = 'catchup_takeover_two_nodes_ahead';
    var replSet = new ReplSetTest({name: name, nodes: 5});
    var nodes = replSet.startSet();
    var config = replSet.getReplSetConfig();
    // Prevent nodes from syncing from other secondaries.
    config.settings = {chainingAllowed: false};
    replSet.initiate(config);
    replSet.awaitReplication();

    // Write something so that nodes 0 and 1 are ahead.
    stopServerReplication(nodes.slice(2, 5));
    var primary = replSet.getPrimary();
    var writeConcern = {writeConcern: {w: 2, wtimeout: replSet.kDefaultTimeoutMS}};
    assert.writeOK(primary.getDB(name).bar.insert({x: 100}, writeConcern));

    // Write something so that node 0 is ahead of node 1.
    stopServerReplication(nodes[1]);
    writeConcern = {writeConcern: {w: 1, wtimeout: replSet.kDefaultTimeoutMS}};
    assert.writeOK(primary.getDB(name).bar.insert({y: 100}, writeConcern));

    // Step up one of the lagged nodes.
    assert.commandWorked(nodes[2].adminCommand({replSetStepUp: 1}));
    replSet.awaitNodesAgreeOnPrimary();
    assert.eq(ReplSetTest.State.PRIMARY,
              assert.commandWorked(nodes[2].adminCommand('replSetGetStatus')).myState,
              nodes[2].host + " was not primary after step-up");
    jsTestLog('node 2 is now primary, but cannot accept writes');

    // Make sure that node 2 cannot write anything. Because it is lagged and replication
    // has been stopped, it shouldn't be able to become master.
    assert.writeErrorWithCode(nodes[2].getDB(name).bar.insert({z: 100}, writeConcern),
                              ErrorCodes.NotMaster);

    // Confirm that the most up-to-date node becomes primary
    // after the default catchup delay.
    replSet.waitForState(0, ReplSetTest.State.PRIMARY, 60 * 1000);

    // Wait until the old primary steps down so the connections won't be closed.
    replSet.waitForState(2, ReplSetTest.State.SECONDARY, replSet.kDefaultTimeoutMS);
    // Let the nodes catchup.
    restartServerReplication(nodes.slice(1, 5));
})();
