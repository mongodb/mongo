/* SERVER-27050 This test causes node 2 to enter rollback, then fail after setting minValid, but
 * before truncating the oplog. It will then choose the same sync source (1) and retry the rollback.
 * The upstream node itself rolls back at this point. Node 2 should detect this case and fail the
 * rollback and refuse to choose node 1 as its sync source because it doesn't have the minValid.
 */

(function() {
    'use strict';

    load("jstests/libs/check_log.js");
    load("jstests/replsets/rslib.js");

    var collName = "test.coll";
    var counter = 0;

    var rst = new ReplSetTest({
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
        {a: counter++}, {writeConcern: {w: 3, wtimeout: rst.kDefaultTimeoutMs}}));

    jsTestLog("Create two partitions: [1] and [0,2,3,4].");
    nodes[1].disconnect(nodes[0]);
    nodes[1].disconnect(nodes[2]);
    nodes[1].disconnect(nodes[3]);
    nodes[1].disconnect(nodes[4]);

    jsTestLog("Do a write that is replicated to [0,2,3,4].");
    assert.writeOK(nodes[0].getCollection(collName).insert(
        {a: counter++}, {writeConcern: {w: 2, wtimeout: rst.kDefaultTimeoutMs}}));

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
    // We track this object to ensure it gets rolled back on node 1 later.
    assert.writeOK(nodes[1].getCollection(collName).insert({a: counter++, tracked: true}));

    // Turn on failpoint on node 2 to pause rollback after oplog is truncated and minValid is set.
    assert.commandWorked(nodes[2].adminCommand(
        {configureFailPoint: 'rollbackHangThenFailAfterWritingMinValid', mode: 'alwaysOn'}));

    jsTestLog("Repartition to: [0] and [1,2,3,4].");
    nodes[2].disconnect(nodes[0]);
    nodes[2].reconnect(nodes[1]);
    nodes[2].reconnect(nodes[3]);
    nodes[2].reconnect(nodes[4]);

    jsTestLog("Wait for node 2 to go into ROLLBACK and start syncing from node 1.");
    // Since nodes 1 and 2 have now diverged, node 2 should go into rollback.
    waitForState(nodes[2], ReplSetTest.State.ROLLBACK);
    rst.awaitSyncSource(nodes[2], nodes[1]);

    jsTestLog("Wait for failpoint on node 2 to pause rollback after it writes minValid");
    // Wait for fail point message to be logged.
    checkLog.contains(nodes[2],
                      'rollback - rollbackHangThenFailAfterWritingMinValid fail point enabled');

    // Switch failpoints, causing rollback to fail then pause when it retries. It is important to
    // enable the new one before disabling the current one.
    assert.commandWorked(
        nodes[2].adminCommand({configureFailPoint: 'rollbackHangBeforeStart', mode: 'alwaysOn'}));
    assert.commandWorked(nodes[2].adminCommand(
        {configureFailPoint: 'rollbackHangThenFailAfterWritingMinValid', mode: 'off'}));
    jsTestLog("Wait for failpoint on node 2 to pause rollback after it restarts");
    // Wait for fail point message to be logged.
    checkLog.contains(nodes[2], 'rollback - rollbackHangBeforeStart fail point enabled');

    jsTestLog("Repartition to: [0,3,4] and [1,2].");
    nodes[3].disconnect(nodes[1]);
    nodes[3].reconnect(nodes[0]);
    nodes[4].disconnect(nodes[1]);
    nodes[4].reconnect(nodes[0]);

    jsTestLog("Ensure that 0 becomes primary.");
    waitForState(nodes[0], ReplSetTest.State.PRIMARY);
    waitForState(nodes[1], ReplSetTest.State.SECONDARY);
    assert.eq(nodes[0], rst.getPrimary());
    // Do a write so that node 0 is definitely ahead of node 1.
    assert.writeOK(nodes[0].getCollection(collName).insert({a: counter++}));

    jsTestLog("Repartition to: [0,1,3,4] and [2] so 1 rolls back and replicates from 0.");
    assert.eq(nodes[1].getCollection(collName).count({tracked: true}), 1);
    nodes[1].reconnect(nodes[0]);
    waitForState(nodes[1], ReplSetTest.State.SECONDARY);
    jsTestLog("w:2 write to node 0");
    assert.writeOK(nodes[0].getCollection(collName).insert(
        {a: counter++}, {writeConcern: {w: 2, wtimeout: rst.kDefaultTimeoutMs}}));
    reconnect(nodes[1]);  // rollback drops connections.
    assert.eq(nodes[1].getCollection(collName).count({tracked: true}), 0);

    // Turn off failpoint on node 2 to allow rollback to finish its attempt at rollback from node 1.
    // It should fail with a rbid error and get stuck.
    jsTestLog("Repartition to: [0,3,4] and [1,2].");
    nodes[1].reconnect(nodes[2]);
    assert.commandWorked(
        nodes[2].adminCommand({configureFailPoint: 'rollbackHangBeforeStart', mode: 'off'}));

    jsTestLog("Wait for node 2 exit ROLLBACK state and go into RECOVERING");
    waitForState(nodes[2], ReplSetTest.State.RECOVERING);

    // At this point node 2 has truncated its oplog back to the common point and is looking
    // for a sync source it can use to reach minvalid and get back into SECONDARY state.  Node 1
    // is the only node it can reach, but since node 1 doesn't contain node 2's minvalid oplog entry
    // node 2 will refuse to use it as a sync source.
    checkLog.contains(nodes[2], "Upstream node rolled back. Need to retry our rollback.");
    waitForState(nodes[2], ReplSetTest.State.RECOVERING);

    // This log message means that it will not be willing to use node 1 as the sync source when it
    // retries.
    checkLog.contains(
        nodes[2], "remote oplog does not contain entry with optime matching our required optime");

    rst.stopSet();
}());
