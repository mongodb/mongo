/*
 * Tests that heartbeats containing writes from a different branch of history can't cause a stale
 * primary to incorrectly acknowledge a w:majority write that's about to be rolled back, even if the
 * stale primary is re-elected primary before waiting for the write concern acknowledgement.
 */
(function() {
    'use strict';

    var name = "writeConcernStepDownAndBackUp";
    var dbName = "wMajorityCheck";
    var collName = "stepdownAndBackUp";

    var rst = new ReplSetTest(
        {name: name, nodes: [{}, {}, {rsConfig: {priority: 0}}, ], useBridge: true});
    var nodes = rst.startSet();
    rst.initiate();

    var timeout = 5 * 60 * 1000;

    function waitForState(node, state) {
        assert.soonNoExcept(function() {
            assert.commandWorked(node.adminCommand(
                {replSetTest: 1, waitForMemberState: state, timeoutMillis: timeout}));
            return true;
        });
    }

    function waitForPrimary(node) {
        assert.soon(function() {
            return node.adminCommand('ismaster').ismaster;
        });
    }

    jsTestLog("Make sure node 0 is primary.");
    rst.stepUp(nodes[0]);
    var primary = rst.getPrimary();
    var secondaries = rst.getSecondaries();
    assert.eq(nodes[0], primary);
    // Wait for all data bearing nodes to get up to date.
    assert.writeOK(nodes[0].getDB(dbName).getCollection(collName).insert(
        {a: 1}, {writeConcern: {w: 3, wtimeout: timeout}}));

    // Stop the secondaries from replicating.
    secondaries.forEach(function(node) {
        assert.commandWorked(
            node.adminCommand({configureFailPoint: 'rsSyncApplyStop', mode: 'alwaysOn'}));
    });
    // Stop the primary from calling into awaitReplication()
    assert.commandWorked(nodes[0].adminCommand(
        {configureFailPoint: 'hangBeforeWaitingForWriteConcern', mode: 'alwaysOn'}));

    jsTestLog("Do w:majority write that won't enter awaitReplication() until after the primary " +
              "has stepped down and back up");
    var doMajorityWrite = function() {
        assert.commandWorked(db.adminCommand({ismaster: 1}));

        assert.throws(function() {
            db.getSiblingDB('wMajorityCheck')
                .stepdownAndBackUp.insert({a: 2}, {writeConcern: {w: 'majority'}});
        });
    };

    var joinMajorityWriter = startParallelShell(doMajorityWrite, nodes[0].port);

    jsTest.log("Disconnect primary from all secondaries");
    nodes[0].disconnect(nodes[1]);
    nodes[0].disconnect(nodes[2]);

    jsTest.log("Wait for a new primary to be elected");
    // Allow the secondaries to replicate again.
    secondaries.forEach(function(node) {
        assert.commandWorked(
            node.adminCommand({configureFailPoint: 'rsSyncApplyStop', mode: 'off'}));
    });

    waitForPrimary(nodes[1]);

    jsTest.log("Do a write to the new primary");
    assert.writeOK(nodes[1].getDB(dbName).getCollection(collName).insert(
        {a: 3}, {writeConcern: {w: 2, wtimeout: timeout}}));

    jsTest.log("Reconnect the old primary to the rest of the nodes");
    nodes[0].reconnect(nodes[1]);
    nodes[0].reconnect(nodes[2]);

    jsTest.log("Wait for the old primary to step down, roll back its write, and apply the " +
               "new writes from the new primary");
    waitForState(nodes[0], ReplSetTest.State.SECONDARY);
    rst.awaitReplication();

    // At this point all 3 nodes should have the same data
    assert.soonNoExcept(function() {
        nodes.forEach(function(node) {
            assert.eq(null,
                      node.getDB(dbName).getCollection(collName).findOne({a: 2}),
                      "Node " + node.host + " contained op that should have been rolled back");
            assert.neq(null,
                       node.getDB(dbName).getCollection(collName).findOne({a: 3}),
                       "Node " + node.host +
                           " was missing op from branch of history that should have persisted");
        });
        return true;
    });

    jsTest.log("Make the original primary become primary once again");
    rst.stepUp(nodes[0]);

    jsTest.log("Unblock the thread waiting for replication of the now rolled-back write, ensure " +
               "that the write concern failed");
    assert.commandWorked(nodes[0].adminCommand(
        {configureFailPoint: 'hangBeforeWaitingForWriteConcern', mode: 'off'}));

    joinMajorityWriter();

    rst.stopSet();
}());
