/**
 * This test tests that replSetGetStatus returns initial sync stats while initial sync is in
 * progress.
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

let name = "initial_sync_replSetGetStatus";
let replSet = new ReplSetTest({
    name: name,
    nodes: 1,
});

replSet.startSet();
replSet.initiate();
let primary = replSet.getPrimary();

const barColl = primary.getDB("pretest").bar;
assert.commandWorked(barColl.insert({a: 1}));
assert.commandWorked(barColl.insert({a: 2}));
assert.commandWorked(barColl.insert({a: 3}));

let coll = primary.getDB("test").foo;
assert.commandWorked(coll.insert({a: 1}));
assert.commandWorked(coll.insert({a: 2}));

// Add a secondary node but make it hang before copying databases.
let secondary = replSet.add({rsConfig: {votes: 0, priority: 0}, setParameter: {"collectionClonerBatchSize": 2}});
secondary.setSecondaryOk();

const failPointBeforeCopying = configureFailPoint(secondary, "initialSyncHangBeforeCopyingDatabases");
const failPointBeforeFinish = configureFailPoint(secondary, "initialSyncHangBeforeFinish");
const failPointAfterFinish = configureFailPoint(secondary, "initialSyncHangAfterFinish");
let failPointAfterNumDocsCopied = configureFailPoint(secondary, "initialSyncHangDuringCollectionClone", {
    namespace: barColl.getFullName(),
    numDocsToClone: 2,
});
replSet.reInitiate();

// Wait for initial sync to pause before it copies the databases.
failPointBeforeCopying.wait();

// Test that replSetGetStatus returns the correct results while initial sync is in progress.
let res = assert.commandWorked(secondary.adminCommand({replSetGetStatus: 1}));
assert(res.initialSyncStatus, () => "Response should have an 'initialSyncStatus' field: " + tojson(res));

res = assert.commandWorked(secondary.adminCommand({replSetGetStatus: 1, initialSync: 0}));
assert(!res.initialSyncStatus, () => "Response should not have an 'initialSyncStatus' field: " + tojson(res));

assert.commandFailedWithCode(secondary.adminCommand({replSetGetStatus: 1, initialSync: "t"}), ErrorCodes.TypeMismatch);

assert.commandWorked(coll.insert({a: 3}));
assert.commandWorked(coll.insert({a: 4}));

// Let initial sync continue working.
failPointBeforeCopying.off();

// Wait for initial sync to pause halfway through cloning the 'pretest.bar' collection.
failPointAfterNumDocsCopied.wait();
const pretestDbRes = assert.commandWorked(secondary.adminCommand({replSetGetStatus: 1}));

assert.gt(pretestDbRes.initialSyncStatus.totalInitialSyncElapsedMillis, 0);
assert.gt(pretestDbRes.initialSyncStatus.remainingInitialSyncEstimatedMillis, 0);
assert.gt(pretestDbRes.initialSyncStatus.approxTotalDataSize, 0);

assert.eq(pretestDbRes.initialSyncStatus.databases.pretest.collections, 1);
assert.eq(pretestDbRes.initialSyncStatus.databases.pretest.clonedCollections, 0);

let barCollRes = pretestDbRes.initialSyncStatus.databases.pretest["pretest.bar"];
assert.eq(barCollRes.documentsToCopy, 3);
// Even though we set the collectionClonerBatchSize to 2, it is possible for a batch to actually
// return only 1 document. This can lead to us hitting the failpoint in the next batch instead,
// causing us to copy up to 3 documents.
assert.lte(barCollRes.documentsCopied, 3);
assert.gt(barCollRes.bytesToCopy, 0);
assert.gt(barCollRes.approxBytesCopied, 0);
assert.lte(barCollRes.approxBytesCopied, barCollRes.bytesToCopy);
assert.lt(barCollRes.approxBytesCopied, pretestDbRes.initialSyncStatus.approxTotalDataSize);

const bytesCopiedAdminDb =
    pretestDbRes.initialSyncStatus.databases.admin["admin.system.version"].approxBytesCopied +
    pretestDbRes.initialSyncStatus.databases.admin["admin.system.keys"].approxBytesCopied;
assert.eq(pretestDbRes.initialSyncStatus.approxTotalBytesCopied, bytesCopiedAdminDb + barCollRes.approxBytesCopied);
assert.gt(pretestDbRes.initialSyncStatus.approxTotalBytesCopied, 0);

// The server still has the 'pretest' and 'test' dbs to finish cloning.
assert.eq(pretestDbRes.initialSyncStatus.databases.databasesCloned, 2);
assert.eq(pretestDbRes.initialSyncStatus.databases.databasesToClone, 2);

failPointAfterNumDocsCopied.off();

// Wait for initial sync to pause right before it finishes.
failPointBeforeFinish.wait();

// Test that replSetGetStatus returns the correct results when initial sync is at the very end.
const endOfCloningRes = assert.commandWorked(secondary.adminCommand({replSetGetStatus: 1}));
assert(endOfCloningRes.initialSyncStatus, () => "Response should have an 'initialSyncStatus' field: " + tojson(res));

// It is possible that we update the config document after going through a reconfig. So make sure
// we account for this.
assert.gte(endOfCloningRes.initialSyncStatus.appliedOps, 3);

// Assert metrics have progressed in the right direction since the last time we checked the metrics.
assert.gt(
    endOfCloningRes.initialSyncStatus.totalInitialSyncElapsedMillis,
    pretestDbRes.initialSyncStatus.totalInitialSyncElapsedMillis,
);
assert.lt(
    endOfCloningRes.initialSyncStatus.remainingInitialSyncEstimatedMillis,
    pretestDbRes.initialSyncStatus.remainingInitialSyncEstimatedMillis,
);
assert.gt(
    endOfCloningRes.initialSyncStatus.approxTotalBytesCopied,
    pretestDbRes.initialSyncStatus.approxTotalBytesCopied,
);
assert.eq(endOfCloningRes.initialSyncStatus.approxTotalDataSize, pretestDbRes.initialSyncStatus.approxTotalDataSize);

assert.eq(endOfCloningRes.initialSyncStatus.failedInitialSyncAttempts, 0);
assert.eq(endOfCloningRes.initialSyncStatus.maxFailedInitialSyncAttempts, 10);

assert.eq(endOfCloningRes.initialSyncStatus.databases.databasesCloned, 4);
assert.eq(endOfCloningRes.initialSyncStatus.databases.databasesToClone, 0);

assert.eq(endOfCloningRes.initialSyncStatus.databases.pretest.collections, 1);
assert.eq(endOfCloningRes.initialSyncStatus.databases.pretest.clonedCollections, 1);
barCollRes = endOfCloningRes.initialSyncStatus.databases.pretest["pretest.bar"];
assert.eq(barCollRes.documentsToCopy, 3);
assert.eq(barCollRes.documentsCopied, 3);
assert.eq(barCollRes.indexes, 1);
assert.eq(barCollRes.fetchedBatches, 2);
assert.gt(barCollRes.bytesToCopy, 0);
assert.eq(barCollRes.approxBytesCopied, barCollRes.bytesToCopy);

let fooCollRes = endOfCloningRes.initialSyncStatus.databases.test["test.foo"];
assert.eq(endOfCloningRes.initialSyncStatus.databases.test.collections, 1);
assert.eq(endOfCloningRes.initialSyncStatus.databases.test.clonedCollections, 1);
assert.eq(fooCollRes.documentsToCopy, 4);
assert.eq(fooCollRes.documentsCopied, 4);
assert.eq(fooCollRes.indexes, 1);
assert.eq(fooCollRes.fetchedBatches, 2);
assert.gt(fooCollRes.bytesToCopy, 0);
assert.eq(fooCollRes.approxBytesCopied, fooCollRes.bytesToCopy);

assert.eq(
    endOfCloningRes.initialSyncStatus.approxTotalDataSize,
    endOfCloningRes.initialSyncStatus.approxTotalBytesCopied,
);
assert.eq(
    endOfCloningRes.initialSyncStatus.approxTotalBytesCopied,
    fooCollRes.approxBytesCopied + barCollRes.approxBytesCopied + bytesCopiedAdminDb,
);

failPointBeforeFinish.off();

// Wait until the 'initialSync' field has been cleared before issuing 'replSetGetStatus'.
failPointAfterFinish.wait();

// Test that replSetGetStatus returns the correct results after initial sync is finished.
res = assert.commandWorked(secondary.adminCommand({replSetGetStatus: 1}));
assert(!res.initialSyncStatus, () => "Response should not have an 'initialSyncStatus' field: " + tojson(res));

assert.commandFailedWithCode(secondary.adminCommand({replSetGetStatus: 1, initialSync: "m"}), ErrorCodes.TypeMismatch);

// Let initial sync finish and get into secondary state.
failPointAfterFinish.off();
replSet.awaitSecondaryNodes(60 * 1000);

assert.eq(
    0,
    secondary.getDB("local")["temp_oplog_buffer"].find().itcount(),
    "Oplog buffer was not dropped after initial sync",
);

replSet.stopSet();
