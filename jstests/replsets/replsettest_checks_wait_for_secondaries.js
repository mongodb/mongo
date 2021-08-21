/**
 * Tests that ReplSetTest consistency checks, namely checkDBHashesForReplSet and
 * checkCollectionCounts, wait for secondaries to have fully transitioned to SECONDARY state before
 * attempting data reads.
 */
(function() {
"use strict";
load("jstests/libs/fail_point_util.js");

const testName = jsTestName();
const dbName = "testdb";
const collName = "testcoll";

const rst = new ReplSetTest({name: testName, nodes: 1});
rst.startSet();
rst.initiateWithHighElectionTimeout();

const primary = rst.getPrimary();
const primaryDb = primary.getDB(dbName);
const primaryColl = primaryDb.getCollection(collName);

assert.commandWorked(primaryColl.insert({"starting": "doc"}));

jsTestLog("Adding a new node to the replica set");
const secondaryParams = {
    'failpoint.initialSyncHangBeforeCopyingDatabases': tojson({mode: 'alwaysOn'}),
    'numInitialSyncAttempts': 1,
};
const secondary = rst.add({rsConfig: {priority: 0}, setParameter: secondaryParams});
rst.reInitiate();

jsTestLog("Waiting for node to reach initial sync");
assert.commandWorked(secondary.adminCommand({
    waitForFailPoint: "initialSyncHangBeforeCopyingDatabases",
    timesEntered: 1,
    maxTimeMS: kDefaultWaitForFailPointTimeout
}));

// Turn off the failpoint and immediately proceeed with collection counts checks.
jsTestLog("Trying checkCollectionCounts");
assert.commandWorked(secondary.adminCommand(
    {configureFailPoint: "initialSyncHangBeforeCopyingDatabases", mode: "off"}));
rst.checkCollectionCounts();

// Restart the node so we can try checkReplicatedDBHashes.
rst.stop(1);
rst.start(1, {startClean: true});

// stopSet() will call checkReplicatedDBHashes
rst.stopSet();
})();