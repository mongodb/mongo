/**
 * Tests readConcern level snapshot outside of transactions.
 *
 * @tags: [
 *   requires_fcv_46,
 *   requires_majority_read_concern,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/global_snapshot_reads_util.js");

const options = {
    // Set a large snapshot window of 10 minutes for the test.
    setParameter: {minSnapshotHistoryWindowInSeconds: 600}
};
const replSet = new ReplSetTest({nodes: 3, nodeOptions: options});
replSet.startSet();
replSet.initiateWithHighElectionTimeout();
let primaryAdmin = replSet.getPrimary().getDB("admin");
assert.eq(assert
              .commandWorked(
                  primaryAdmin.runCommand({getParameter: 1, minSnapshotHistoryWindowInSeconds: 1}))
              .minSnapshotHistoryWindowInSeconds,
          600);
const primaryDB = replSet.getPrimary().getDB('test');
const secondaryDB = replSet.getSecondary().getDB('test');
const snapshotReadsTest = new SnapshotReadsTest({
    primaryDB: primaryDB,
    secondaryDB: secondaryDB,
    awaitCommittedFn: () => {
        replSet.awaitLastOpCommitted();
    }
});

snapshotReadsTest.cursorTest({testScenarioName: jsTestName(), collName: "test"});
snapshotReadsTest.distinctTest({testScenarioName: jsTestName(), collName: "test"});
snapshotReadsTest.outAndMergeTest(
    {testScenarioName: jsTestName(), coll: "test", outColl: "testOut", isOutCollSharded: false});
snapshotReadsTest.lookupAndUnionWithTest(
    {testScenarioName: jsTestName(), coll1: "test1", coll2: "test2", isColl2Sharded: false});

// Ensure "atClusterTime" is omitted from a regular (non-snapshot) read.
primaryDB["collection"].insertOne({});
const cursor = assert.commandWorked(primaryDB.runCommand({find: "test"})).cursor;
assert(!cursor.hasOwnProperty("atClusterTime"));
const distinctResult = assert.commandWorked(primaryDB.runCommand({distinct: "test", key: "_id"}));
assert(!distinctResult.hasOwnProperty("atClusterTime"));

replSet.stopSet();
})();
