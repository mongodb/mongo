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
const primaryDB = replSet.getPrimary().getDB('test');
const secondaryDB = replSet.getSecondary().getDB('test');
snapshotReadsTest({
    testScenarioName: jsTestName(),
    primaryDB: primaryDB,
    secondaryDB: secondaryDB,
    collName: "test",
    awaitCommittedFn: () => {
        replSet.awaitLastOpCommitted();
    }
});

// Ensure "atClusterTime" is omitted from a regular (non-snapshot) read.
primaryDB["collection"].insertOne({});
const cursor = assert.commandWorked(primaryDB.runCommand({find: "test"})).cursor;
assert(!cursor.hasOwnProperty("atClusterTime"));
const distinctResult = assert.commandWorked(primaryDB.runCommand({distinct: "test", key: "_id"}));
assert(!distinctResult.hasOwnProperty("atClusterTime"));

replSet.stopSet();
})();
