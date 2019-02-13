/**
 * Tests that a node can be successfully restarted when the bridge is enabled. Also verifies the
 * bridge configuration is left intact even after the node is restarted.
 *
 * @tags: [requires_persistence, requires_replication]
 */
(function() {
    "use strict";

    load("jstests/replsets/rslib.js");  // for reconnect

    const rst = new ReplSetTest({
        nodes: [{}, {rsConfig: {priority: 0, votes: 0}}],
        useBridge: true,
    });

    rst.startSet();
    rst.initiate();
    rst.awaitNodesAgreeOnPrimary();

    const primary = rst.getPrimary();
    const secondary = rst.getSecondary();

    const primaryDB = primary.getDB("test");
    const primaryColl = primaryDB.getCollection("restart_node_with_bridge");

    function assertWriteReplicates() {
        assert.commandWorked(primaryColl.update(
            {_id: 0}, {$inc: {counter: 1}}, {upsert: true, writeConcern: {w: 2}}));
    }

    function assertWriteFailsToReplicate() {
        assert.commandFailedWithCode(
            primaryColl.update(
                {_id: 0}, {$inc: {counter: 1}}, {writeConcern: {w: 2, wtimeout: 1000}}),
            ErrorCodes.WriteConcernFailed);
    }

    // By default, the primary should be connected to the secondary. Replicating a write should
    // therefore succeed.
    assertWriteReplicates();

    // We disconnect the primary from the secondary and verify that replicating a write fails.
    primary.disconnect(secondary);
    assertWriteFailsToReplicate();

    // We restart the secondary and verify that replicating a write still fails.
    rst.restart(secondary);
    assertWriteFailsToReplicate();

    // We restart the primary and verify that replicating a write still fails.
    rst.restart(primary);
    rst.getPrimary();
    // Note that we specify 'primaryDB' to avoid having reconnect() send a message directly to the
    // mongod process rather than going through the mongobridge process as well.
    reconnect(primaryDB);
    assertWriteFailsToReplicate();

    // We reconnect the primary to the secondary and verify that replicating a write succeeds.
    primary.reconnect(secondary);
    assertWriteReplicates();

    rst.stopSet();
}());
