/**
 * Tests that an initial sync that starts in the middle of a large transaction on a secondary
 * is able to complete and apply the entire transaction.
 *
 * @tags: [
 *   exclude_from_large_txns,
 *   uses_transactions,
 * ]
 */
(function() {
"use strict";
load("jstests/replsets/rslib.js");  // For reconnect()
load("jstests/libs/fail_point_util.js");

const replTest = new ReplSetTest({
    nodes: [{}, {rsConfig: {priority: 0}}],
    nodeOptions: {
        setParameter:
            // We want two entries in each oplog batch, so the beginning of the transaction is not
            // the end of the batch.
            {maxNumberOfTransactionOperationsInSingleOplogEntry: 1, bgSyncOplogFetcherBatchSize: 2}
    },
});

replTest.startSet();
replTest.initiateWithHighElectionTimeout();

// We have to add and pause the initial sync node here, because we cannot re-initiate the set while
// the failpoints are held.
jsTestLog("Adding initial sync node");
const initialSyncNode = replTest.add({
    rsConfig: {priority: 0, votes: 0},
    setParameter: {
        'initialSyncSourceReadPreference': 'secondary',
        'failpoint.initialSyncHangBeforeChoosingSyncSource': tojson({mode: 'alwaysOn'}),
        'logComponentVerbosity': tojsononeline({replication: {initialSync: 1}})
    }
});
replTest.reInitiate();

const dbName = jsTest.name();
const collName = "coll";

const primary = replTest.getPrimary();
const testDB = primary.getDB(dbName);
const syncSource = replTest.nodes[1];

assert.commandWorked(testDB.runCommand({create: collName, writeConcern: {w: "majority"}}));

jsTest.log("Stop secondary oplog replication before the last operation in the transaction.");
// The stopReplProducerOnDocument failpoint ensures that secondary stops replicating before
// accepting the last two operations in the transaction.
const stopReplProducerOnDocumentFailPoint = configureFailPoint(
    syncSource, "stopReplProducerOnDocument", {document: {"applyOps.o._id": "next-last in txn"}});

// This will cause us to pause batch application at a critical point, with "first in txn" and
// "next in txn" in the oplog but not applied.
const pauseBatchApplicationAfterWritingOplogEntriesFailPoint =
    configureFailPoint(syncSource, "pauseBatchApplicationAfterWritingOplogEntries");

jsTestLog("Starting transaction");
const session = primary.startSession({causalConsistency: false});
const sessionDB = session.getDatabase(dbName);
session.startTransaction({writeConcern: {w: 1}});

assert.commandWorked(sessionDB.getCollection(collName).insert({_id: "first in txn"}));
assert.commandWorked(sessionDB.getCollection(collName).insert({_id: "next in txn"}));
assert.commandWorked(sessionDB.getCollection(collName).insert({_id: "next-last in txn"}));
assert.commandWorked(sessionDB.getCollection(collName).insert({_id: "last in txn"}));

jsTestLog("Committing transaction");
assert.commandWorked(session.commitTransaction_forTesting());
stopReplProducerOnDocumentFailPoint.wait();
pauseBatchApplicationAfterWritingOplogEntriesFailPoint.wait();

jsTestLog("Starting initial sync");
assert.commandWorked(initialSyncNode.adminCommand(
    {configureFailPoint: "initialSyncHangBeforeChoosingSyncSource", mode: "off"}));

// The bug we're testing for is a race.  The way we fix the race is to wait until it resolves.
// So it's very difficult to test it deterministically; if we use failpoints to force the race to
// happen, we'll just hang when the code is correct.  Instead we hold the sync source in the
// condition that causes the race until we reach the point it's about to happen, then release
// it hoping it'll fail most of the time when the bug is present.
jsTestLog("Waiting to fetch the defaultBeginFetchingOplogEntry");
checkLog.containsJson(initialSyncNode, 6608900);
stopReplProducerOnDocumentFailPoint.off();
pauseBatchApplicationAfterWritingOplogEntriesFailPoint.off();
replTest.awaitSecondaryNodes();
replTest.awaitReplication();

// Make sure we got the transaction.
assert.eq(4, initialSyncNode.getDB(dbName)[collName].find().itcount());

printjson(initialSyncNode.getDB("local").oplog.rs.find().toArray());

replTest.stopSet();
})();
