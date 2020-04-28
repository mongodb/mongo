/* Tests readConcern level snapshot outside of transactions.
 *
 * TODO(SERVER-46592): This test is multiversion-incompatible in 4.6.  If we use 'requires_fcv_46'
 *                     as the tag for that, removing 'requires_fcv_44' is sufficient.  Otherwise,
 *                     please set the appropriate tag when removing 'requires_fcv_44'
 * @tags: [requires_majority_read_concern, requires_fcv_44, requires_fcv_46]
 */
(function() {
"use strict";

load("jstests/libs/global_snapshot_reads_util.js");

// TODO(SERVER-47672): Use minSnapshotHistoryWindowInSeconds instead.
const options = {
    setParameter: "maxTargetSnapshotHistoryWindowInSeconds=600",
};
const replSet = new ReplSetTest({nodes: 3, nodeOptions: options});
replSet.startSet();
replSet.initiateWithHighElectionTimeout();
const primary = replSet.getPrimary();
const testDB = primary.getDB('test');
snapshotReadsTest(jsTestName(), testDB, "test");

// Ensure "atClusterTime" is omitted from a regular (non-snapshot) cursor.
testDB["collection"].insertOne({});
const cursor = assert.commandWorked(testDB.runCommand({find: "collection"})).cursor;
assert(!cursor.hasOwnProperty("atClusterTime"));

replSet.stopSet();
})();
