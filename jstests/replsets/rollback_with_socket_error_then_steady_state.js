// This test causes node 2 to enter rollback and then fail with a SocketException before updating
// MinValid or altering durable state in any way. It will then choose a sync source from which it
// is able to stitch the oplog and therefore doesn't need to roll back. Prior to SERVER-27282, the
// node would be "stuck" with state=ROLLBACK while it was doing steady-state replication, with no
// way to reach SECONDARY without restarting the process.
(function() {
    'use strict';

    load("jstests/libs/check_log.js");
    load("jstests/replsets/rslib.js");

    var collName = "test.coll";
    var counter = 0;

    var rst = new ReplSetTest({
        name: 'rollback_with_socket_error_then_steady_state',
        nodes: [
            // Primary flops between nodes 0 and 1.
            {},
            {},
            // Node 2 is the node under test.
            {rsConfig: {priority: 0}},
            // Arbiters to sway elections.
            {rsConfig: {arbiterOnly: true}},
            {rsConfig: {arbiterOnly: true}}
        ],
        useBridge: true
    });
    var nodes = rst.startSet();
    rst.initiate();

    function stepUp(rst, node) {
        var primary = rst.getPrimary();
        if (primary != node) {
            try {
                assert.commandWorked(primary.adminCommand({replSetStepDown: 1, force: true}));
            } catch (ex) {
                print("Caught exception while stepping down from node '" + tojson(node.host) +
                      "': " + tojson(ex));
            }
        }
        waitForState(node, ReplSetTest.State.PRIMARY);
    }

    jsTestLog("Make sure node 0 is primary.");
    stepUp(rst, nodes[0]);
    assert.eq(nodes[0], rst.getPrimary());
    // Wait for all data bearing nodes to get up to date.
    assert.writeOK(nodes[0].getCollection(collName).insert(
        {a: counter++}, {writeConcern: {w: 3, wtimeout: 5 * 60 * 1000}}));

    jsTestLog("Create two partitions: [1] and [0,2,3,4].");
    nodes[1].disconnect(nodes[0]);
    nodes[1].disconnect(nodes[2]);
    nodes[1].disconnect(nodes[3]);
    nodes[1].disconnect(nodes[4]);

    jsTestLog("Do a write that is replicated to [0,2,3,4].");
    assert.writeOK(nodes[0].getCollection(collName).insert(
        {a: counter++}, {writeConcern: {w: 2, wtimeout: 5 * 60 * 1000}}));

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
    assert.writeOK(nodes[1].getCollection(collName).insert({a: counter++}));

    // Turn on failpoint on node 2 to pause rollback before doing anything.
    assert.commandWorked(
        nodes[2].adminCommand({configureFailPoint: 'rollbackHangBeforeStart', mode: 'alwaysOn'}));

    jsTestLog("Repartition to: [0] and [1,2,3,4].");
    nodes[2].disconnect(nodes[0]);
    nodes[2].reconnect(nodes[1]);
    nodes[2].reconnect(nodes[3]);
    nodes[2].reconnect(nodes[4]);

    jsTestLog("Wait for node 2 to decide to go into ROLLBACK and start syncing from node 1.");
    // Since nodes 1 and 2 have now diverged, node 2 should go into rollback. The failpoint will
    // stop it from actually transitioning to rollback, so the checkLog bellow will ensure that we
    // have decided to rollback, but haven't actually started yet.
    rst.awaitSyncSource(nodes[2], nodes[1]);

    jsTestLog("Wait for failpoint on node 2 to pause rollback before it starts");
    // Wait for fail point message to be logged.
    checkLog.contains(nodes[2], 'rollback - rollbackHangBeforeStart fail point enabled');

    jsTestLog("Repartition to: [1] and [0,2,3,4].");
    nodes[1].disconnect(nodes[3]);
    nodes[1].disconnect(nodes[4]);
    nodes[2].disconnect(nodes[1]);
    nodes[2].reconnect(nodes[0]);
    nodes[3].reconnect(nodes[0]);
    nodes[3].reconnect(nodes[2]);
    nodes[4].reconnect(nodes[0]);
    nodes[4].reconnect(nodes[2]);

    // Turn off failpoint on node 2 to allow rollback against node 1 to fail with a network error.
    assert.commandWorked(
        nodes[2].adminCommand({configureFailPoint: 'rollbackHangBeforeStart', mode: 'off'}));

    // Make node 0 ahead of node 2 again so node 2 will pick it as a sync source.

    jsTestLog("waiting for node 0 to be primary");
    waitForState(nodes[1], ReplSetTest.State.SECONDARY);
    waitForState(nodes[0], ReplSetTest.State.PRIMARY);
    assert.eq(nodes[0], rst.getPrimary());

    jsTestLog("w:2 write to node 0 (replicated to node 2)");
    assert.writeOK(nodes[0].getCollection(collName).insert(
        {a: counter++}, {writeConcern: {w: 2, wtimeout: 5 * 60 * 1000}}));

    // At this point node 2 has failed rollback before making any durable changes, including writing
    // to minValid. That means that it is free to pick any sync source and will pick node 0 where it
    // can pick up where it left off without rolling back. Ensure that it is able to reach SECONDARY
    // and doesn't do steady-state replication in ROLLBACK state.
    jsTestLog("Wait for node 2 to go into SECONDARY");
    assert.neq(nodes[2].adminCommand('replSetGetStatus').myState,
               ReplSetTest.State.ROLLBACK,
               "node 2 is doing steady-state replication with state=ROLLBACK!");
    waitForState(nodes[2], ReplSetTest.State.SECONDARY);
}());
