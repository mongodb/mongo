/*
 * This tests that nodes get a new sync source if their sync source rolls back between when it is
 * chosen as a sync source and when it is first used. The test sets up a five node replicaset
 * and creates a simple rollback scenario. Before node 0 goes into rollback, however, we pause
 * the oplog fetcher on node 2, which is syncing from node 0. After the rollback occurs we let
 * the oplog fetcher continue and after it gets back its first batch from the sync source it
 * realizes that its sync source has rolled back and it errors before getting a new sync source.
 */

(function() {
    'use strict';

    load("jstests/libs/check_log.js");
    load("jstests/replsets/rslib.js");

    var name = "rollback_after_sync_source_selection";
    var collName = "test.coll";

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

    jsTestLog("Make sure node 0 is primary.");
    rst.stepUp(nodes[0]);
    assert.eq(nodes[0], rst.getPrimary());
    // Wait for all data bearing nodes to get up to date.
    assert.writeOK(nodes[0].getCollection(collName).insert(
        {a: 0}, {writeConcern: {w: 3, wtimeout: rst.kDefaultTimeoutMS}}));

    jsTestLog("Create two partitions: [1] and [0,2,3,4].");
    nodes[1].disconnect(nodes[0]);
    nodes[1].disconnect(nodes[2]);
    nodes[1].disconnect(nodes[3]);
    nodes[1].disconnect(nodes[4]);

    jsTestLog("Do a write that replicates to [0,2,3,4].");
    assert.writeOK(nodes[0].getCollection(collName).insert(
        {a: 1}, {writeConcern: {w: 2, wtimeout: rst.kDefaultTimeoutMS}}));

    jsTestLog("Pausing node 2's oplog fetcher before first fetch.");
    assert.commandWorked(nodes[2].getDB('admin').runCommand(
        {configureFailPoint: 'fetcherHangBeforeStart', mode: 'alwaysOn'}));
    syncFrom(nodes[2], nodes[0], rst);
    checkLog.contains(nodes[2], 'fetcherHangBeforeStart fail point enabled');

    jsTestLog("Do a write on partition [0,2,3,4]; it won't replicate due to the failpoint.");
    assert.writeOK(nodes[0].getCollection(collName).insert({a: 2}));

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
    assert.writeOK(nodes[1].getCollection(collName).insert({a: 3}));

    jsTestLog("Remove the partition but maintain that node 2 only talks to node 0.");
    var node0RBID = nodes[0].adminCommand('replSetGetRBID').rbid;
    nodes[0].reconnect(nodes[1]);
    nodes[0].reconnect(nodes[3]);
    nodes[0].reconnect(nodes[4]);

    jsTestLog("Wait for node 0 to go into ROLLBACK");
    // Wait for a rollback to happen.
    assert.soonNoExcept(function() {
        var node0RBIDNew = nodes[0].adminCommand('replSetGetRBID').rbid;
        return node0RBIDNew !== node0RBID;
    });
    waitForState(nodes[0], ReplSetTest.State.SECONDARY);

    // At this point nodes 0 and 1 should have the same data.
    assert.neq(null,
               nodes[0].getCollection(collName).findOne({a: 0}),
               "Node " + nodes[0].host +
                   " did not contain initial op that should be present on all nodes");
    assert.eq(null,
              nodes[0].getCollection(collName).findOne({a: 1}),
              "Node " + nodes[0].host + " contained op that should have been rolled back");
    assert.eq(null,
              nodes[0].getCollection(collName).findOne({a: 2}),
              "Node " + nodes[0].host + " contained op that should have been rolled back");
    assert.neq(null,
               nodes[0].getCollection(collName).findOne({a: 3}),
               "Node " + nodes[0].host + " did not contain op from after rollback");

    jsTestLog("Let oplog fetcher continue and error that the sync source rolled back.");
    // Turn off failpoint on node 2 to allow it to continue fetching.
    assert.commandWorked(nodes[2].getDB('admin').runCommand(
        {configureFailPoint: 'fetcherHangBeforeStart', mode: 'off'}));
    checkLog.contains(nodes[2],
                      "Upstream node rolled back after verifying that it had our MinValid point.");

    rst.awaitSecondaryNodes();
    rst.stopSet();
}());
