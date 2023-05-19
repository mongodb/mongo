/**
 * This test aims to evaluate the stability of the network bridge.
 * This test replicates the steps of another test, which failed on multiple occasions due to
 * network-related problems.
 */

(function() {
"use strict";

load('jstests/replsets/rslib.js');

const testName = jsTestName();
const dbName = "testdb";
const collName = "testcoll";

for (var test = 0; test < 10; test++) {
    const rst = new ReplSetTest({
        name: testName,
        nodes: [{}, {}, {rsConfig: {priority: 0}}],
        settings: {chainingAllowed: false},
        useBridge: true
    });
    rst.startSet();
    rst.initiateWithHighElectionTimeout();

    const primary = rst.getPrimary();
    const primaryDb = primary.getDB(dbName);
    const primaryColl = primaryDb.getCollection(collName);

    jsTestLog("Adding some data to the primary.");
    assert.commandWorked(primaryColl.insert({"starting": "doc"}, {writeConcern: {w: 3}}));

    jsTestLog("Restarting node 2 so it will do a logical initial sync.");
    rst.restart(rst.nodes[2], {startClean: true});
    rst.awaitReplication();

    jsTestLog("Stepping down the primary");
    assert.commandWorked(primary.adminCommand({replSetStepDown: 60}));

    // Trigger an election that requires node 2's vote.
    jsTestLog("Disconnect node 0 and node 1.");
    rst.nodes[0].disconnect(rst.nodes[1]);

    jsTestLog("Electing node 1 -- this should succeed as it should get node 2's vote.");
    assert.commandWorked(rst.nodes[1].adminCommand({replSetStepUp: 1}));

    // Don't let the initial sync node get oplog from the new primary.
    jsTestLog("Disconnect node 1 and node 2.");
    rst.nodes[2].disconnect(rst.nodes[1]);

    jsTestLog("Connecting all nodes.");
    rst.nodes[0].reconnect(rst.nodes);
    rst.nodes[1].reconnect(rst.nodes);
    rst.nodes[2].reconnect(rst.nodes);

    rst.stopSet();
}
})();
