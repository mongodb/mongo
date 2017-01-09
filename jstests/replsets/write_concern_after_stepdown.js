/*
 * Tests that heartbeats containing writes from a different branch of history can't cause a stale
 * primary to incorrectly acknowledge a w:majority write that's about to be rolled back.
 */
(function() {
    'use strict';

    load("jstests/replsets/rslib.js");
    load("jstests/libs/write_concern_util.js");

    var name = "writeConcernStepDownAndBackUp";
    var dbName = "wMajorityCheck";
    var collName = "stepdownAndBackUp";

    var rst = new ReplSetTest({
        name: name,
        nodes: [
            {},
            {},
            {rsConfig: {priority: 0}},
        ],
        useBridge: true
    });
    var nodes = rst.startSet();
    rst.initiate();

    function waitForPrimary(node) {
        assert.soon(function() {
            return node.adminCommand('ismaster').ismaster;
        });
    }

    // SERVER-20844 ReplSetTest starts up a single node replica set then reconfigures to the correct
    // size for faster startup, so nodes[0] is always the first primary.
    jsTestLog("Make sure node 0 is primary.");
    var primary = rst.getPrimary();
    var secondaries = rst.getSecondaries();
    assert.eq(nodes[0], primary);
    // Wait for all data bearing nodes to get up to date.
    assert.writeOK(nodes[0].getDB(dbName).getCollection(collName).insert(
        {a: 1}, {writeConcern: {w: 3, wtimeout: rst.kDefaultTimeoutMS}}));

    // Stop the secondaries from replicating.
    stopServerReplication(secondaries);
    // Stop the primary from being able to complete stepping down.
    assert.commandWorked(
        nodes[0].adminCommand({configureFailPoint: 'blockHeartbeatStepdown', mode: 'alwaysOn'}));

    jsTestLog("Do w:majority write that will block waiting for replication.");
    var doMajorityWrite = function() {
        var res = db.getSiblingDB('wMajorityCheck').stepdownAndBackUp.insert({a: 2}, {
            writeConcern: {w: 'majority'}
        });
        assert.writeErrorWithCode(res, ErrorCodes.PrimarySteppedDown);
    };

    var joinMajorityWriter = startParallelShell(doMajorityWrite, nodes[0].port);

    jsTest.log("Disconnect primary from all secondaries");
    nodes[0].disconnect(nodes[1]);
    nodes[0].disconnect(nodes[2]);

    jsTest.log("Wait for a new primary to be elected");
    // Allow the secondaries to replicate again.
    restartServerReplication(secondaries);

    waitForPrimary(nodes[1]);

    jsTest.log("Do a write to the new primary");
    assert.writeOK(nodes[1].getDB(dbName).getCollection(collName).insert(
        {a: 3}, {writeConcern: {w: 2, wtimeout: rst.kDefaultTimeoutMS}}));

    jsTest.log("Reconnect the old primary to the rest of the nodes");
    // Only allow the old primary to connect to the other nodes, not the other way around.
    // This is so that the old priamry will detect that it needs to step down and step itself down,
    // rather than one of the other nodes detecting this and sending it a replSetStepDown command,
    // which would cause the old primary to kill all operations and close all connections, making
    // the way that the insert in the parallel shell fails be nondeterministic.  Rather than
    // handling all possible failure modes in the parallel shell, allowing heartbeat connectivity in
    // only one direction makes it easier for the test to fail deterministically.
    nodes[1].acceptConnectionsFrom(nodes[0]);
    nodes[2].acceptConnectionsFrom(nodes[0]);

    joinMajorityWriter();

    // Allow the old primary to finish stepping down so that shutdown can finish.
    var res = null;
    try {
        res = nodes[0].adminCommand({configureFailPoint: 'blockHeartbeatStepdown', mode: 'off'});
    } catch (e) {
        // Expected - once we disable the fail point the stepdown will proceed and it's racy whether
        // the stepdown closes all connections before or after the configureFailPoint command
        // returns
    }
    if (res) {
        assert.commandWorked(res);
    }

    rst.stopSet();
}());
