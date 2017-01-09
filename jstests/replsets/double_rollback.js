/*
 * This test causes node 2 to enter rollback, reach the common point, and exit rollback, but before
 * it can apply operations to bring it back to a consistent state, switch sync sources to the node
 * that originally gave it the ops it is now rolling back (node 0).  This test then verifies that
 * node 2 refuses to use node0 as a sync source because it doesn't contain the minValid document
 * it needs to reach consistency.  Node 2 is then allowed to reconnect to the node it was
 * originally rolling back against (node 1) and finish its rollback.  This is a regression test
 * against the case where we *did* allow node 2 to sync from node 0 which gave it the very ops
 * it rolled back, which could then lead to a double-rollback when node 2 was reconnected
 * to node 1 and tried to apply its oplog despite not being in a consistent state.
 */

(function() {
    'use strict';

    load("jstests/libs/check_log.js");
    load("jstests/replsets/rslib.js");

    var name = "double_rollback";
    var dbName = "test";
    var collName = "double_rollback";

    var rst = new ReplSetTest({
        name: name,
        nodes: [
            {},
            {},
            {rsConfig: {priority: 0}},
            {rsConfig: {arbiterOnly: true}},
            {rsConfig: {arbiterOnly: true}}
        ],
        useBridge: true
    });
    var nodes = rst.startSet();
    rst.initiate();

    // SERVER-20844 ReplSetTest starts up a single node replica set then reconfigures to the correct
    // size for faster startup, so nodes[0] is always the first primary.
    jsTestLog("Make sure node 0 is primary.");
    assert.eq(nodes[0], rst.getPrimary());
    // Wait for all data bearing nodes to get up to date.
    assert.writeOK(nodes[0].getDB(dbName).getCollection(collName).insert(
        {a: 1}, {writeConcern: {w: 3, wtimeout: 5 * 60 * 1000}}));

    jsTestLog("Create two partitions: [1] and [0,2,3,4].");
    nodes[1].disconnect(nodes[0]);
    nodes[1].disconnect(nodes[2]);
    nodes[1].disconnect(nodes[3]);
    nodes[1].disconnect(nodes[4]);

    jsTestLog("Do a write that is replicated to [0,2,3,4].");
    assert.writeOK(nodes[0].getDB(dbName).getCollection(collName + "2").insert({a: 2}, {
        writeConcern: {w: 2, wtimeout: 5 * 60 * 1000}
    }));

    jsTestLog("Repartition to: [0,2] and [1,3,4].");
    nodes[1].reconnect(nodes[3]);
    nodes[1].reconnect(nodes[4]);
    nodes[3].disconnect(nodes[0]);
    nodes[3].disconnect(nodes[2]);
    nodes[4].disconnect(nodes[0]);
    nodes[4].disconnect(nodes[2]);

    jsTestLog("Ensure that 0 steps down and that 1 becomes primary.");
    waitForState(nodes[0], ReplSetTest.State.SECONDARY);
    waitForState(nodes[1], ReplSetTest.State.PRIMARY);
    assert.eq(nodes[1], rst.getPrimary());

    jsTestLog("Do a write to node 1 on the [1,3,4] side of the partition.");
    assert.writeOK(nodes[1].getDB(dbName).getCollection(collName + "3").insert({a: 3}));

    // Turn on failpoint on node 2 to pause rollback after oplog is truncated and minValid is set.
    assert.commandWorked(nodes[2].getDB('admin').runCommand(
        {configureFailPoint: 'rollbackHangBeforeFinish', mode: 'alwaysOn'}));

    jsTestLog("Repartition to: [0] and [1,2,3,4].");
    nodes[2].disconnect(nodes[0]);
    nodes[2].reconnect(nodes[1]);
    nodes[2].reconnect(nodes[3]);
    nodes[2].reconnect(nodes[4]);

    jsTestLog("Wait for node 2 to go into ROLLBACK and start syncing from node 1.");
    // Since nodes 1 and 2 have now diverged, node 2 should go into rollback.
    waitForState(nodes[2], ReplSetTest.State.ROLLBACK);
    rst.awaitSyncSource(nodes[2], nodes[1]);

    jsTestLog("Wait for failpoint on node 2 to pause rollback before it finishes");
    // Wait for fail point message to be logged.
    checkLog.contains(nodes[2], 'rollback - rollbackHangBeforeFinish fail point enabled');

    jsTestLog("Repartition to: [1,3,4] and [0,2].");
    nodes[2].disconnect(nodes[1]);
    nodes[2].reconnect(nodes[0]);

    // Turn off failpoint on node 2 to allow rollback to finish.
    assert.commandWorked(nodes[2].getDB('admin').runCommand(
        {configureFailPoint: 'rollbackHangBeforeFinish', mode: 'off'}));

    jsTestLog("Wait for node 2 exit ROLLBACK state and go into RECOVERING");
    waitForState(nodes[2], ReplSetTest.State.RECOVERING);

    // At this point node 2 has truncated its oplog back to the common point and is looking
    // for a sync source it can use to reach minvalid and get back into SECONDARY state.  Node 0
    // is the only node it can reach, but since node 0 doesn't contain node 2's minvalid oplog entry
    // node 2 will refuse to use it as a sync source.
    checkLog.contains(
        nodes[2], "remote oplog does not contain entry with optime matching our required optime");

    var node0RBID = nodes[0].adminCommand('replSetGetRBID').rbid;
    var node1RBID = nodes[1].adminCommand('replSetGetRBID').rbid;

    jsTestLog("Reconnect all nodes.");
    nodes[0].reconnect(nodes[1]);
    nodes[0].reconnect(nodes[3]);
    nodes[0].reconnect(nodes[4]);
    nodes[2].reconnect(nodes[1]);
    nodes[2].reconnect(nodes[3]);
    nodes[2].reconnect(nodes[4]);

    jsTestLog("Wait for nodes 0 to roll back and both node 0 and 2 to catch up to node 1");
    waitForState(nodes[0], ReplSetTest.State.SECONDARY);
    waitForState(nodes[2], ReplSetTest.State.SECONDARY);
    rst.awaitReplication();

    // Check that rollback happened on node 0, but not on node 2 since it had already rolled back
    // and just needed to finish applying ops to reach minValid.
    assert.neq(node0RBID, nodes[0].adminCommand('replSetGetRBID').rbid);
    assert.eq(node1RBID, nodes[1].adminCommand('replSetGetRBID').rbid);

    // Node 1 should still be primary, and should now be able to satisfy majority writes again.
    assert.writeOK(nodes[1].getDB(dbName).getCollection(collName + "4").insert({a: 4}, {
        writeConcern: {w: 3, wtimeout: 5 * 60 * 1000}
    }));

}());
