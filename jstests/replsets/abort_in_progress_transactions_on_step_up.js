/**
 * Tests primary aborts in-progress transactions on stepup.
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

function getTxnTableEntry(db) {
    let txnTableEntries = db.getSiblingDB("config")["transactions"].find().toArray();
    assert.eq(txnTableEntries.length, 1);
    return txnTableEntries[0];
}

const replTest = new ReplSetTest({
    nodes: 3,
    nodeOptions: {
        setParameter:
            {maxNumberOfTransactionOperationsInSingleOplogEntry: 1, bgSyncOplogFetcherBatchSize: 1}
    },
});

replTest.startSet();
let config = replTest.getReplSetConfig();
config.members[2].priority = 0;
// Disable primary catchup and chaining.
config.settings = {
    catchUpTimeoutMillis: 0,
    chainingAllowed: false
};
replTest.initiate(config);

setLogVerbosity(replTest.nodes, {"replication": {"verbosity": 3}});

const dbName = jsTest.name();
const collName = "coll";

const primary = replTest.nodes[0];
const testDB = primary.getDB(dbName);
const newPrimary = replTest.nodes[1];
const newTestDB = newPrimary.getDB(dbName);

testDB.dropDatabase();
assert.commandWorked(testDB.runCommand({create: collName, writeConcern: {w: "majority"}}));

// Prevent the priority: 0 node from fetching new ops so that it can vote for the new primary.
const stopReplProducerFailPoint = configureFailPoint(replTest.nodes[2], 'stopReplProducer');

jsTest.log("Stop secondary oplog replication before the last operation in the transaction.");
// The stopReplProducerOnDocument failpoint ensures that secondary stops replicating before
// applying the last operation in the transaction. This depends on the oplog fetcher batch size
// being 1.
const stopReplProducerOnDocumentFailPoint = configureFailPoint(
    newPrimary, "stopReplProducerOnDocument", {document: {"applyOps.o._id": "last in txn"}});

jsTestLog("Starting transaction");
const session = primary.startSession({causalConsistency: false});
const sessionDB = session.getDatabase(dbName);
session.startTransaction({writeConcern: {w: "majority", wtimeout: 500}});

const doc = {
    _id: "first in txn on primary " + primary
};
assert.commandWorked(sessionDB.getCollection(collName).insert(doc));
assert.commandWorked(sessionDB.getCollection(collName).insert({_id: "last in txn"}));

jsTestLog("Committing transaction but fail on replication");
let res = session.commitTransaction_forTesting();
assert.commandFailedWithCode(res, ErrorCodes.WriteConcernFailed);

// Remember the start and commit OpTimes on primary.
let txnTableEntry = getTxnTableEntry(testDB);
assert.eq(txnTableEntry.state, "committed");
const commitOpTime = txnTableEntry.lastWriteOpTime;
const startOpTime = testDB.getSiblingDB("local").oplog.rs.findOne({ts: commitOpTime.ts}).prevOpTime;

jsTestLog("Wait for the new primary to block on fail point.");
stopReplProducerOnDocumentFailPoint.wait();

jsTestLog("Wait for the new primary to apply the first op of transaction at timestamp: " +
          tojson(startOpTime));
assert.soon(() => {
    const lastOpTime = getLastOpTime(newPrimary);
    jsTestLog("Current lastOpTime on the new primary: " + tojson(lastOpTime));
    return lastOpTime >= startOpTime;
});

// Now the transaction should be in-progress on newPrimary.
txnTableEntry = getTxnTableEntry(newTestDB);
assert.eq(txnTableEntry.state, "inProgress");
// The startOpTime should be less than the commit optime.
assert.eq(rs.compareOpTimes(txnTableEntry.startOpTime, commitOpTime), -1);

jsTestLog("Stepping down primary via heartbeat.");
assert.commandWorked(newPrimary.adminCommand({replSetStepUp: 1}));
replTest.awaitNodesAgreeOnPrimary();
reconnect(primary);

// Make sure we won't apply the whole transaction by any chance.
jsTestLog("Wait for the new primary to stop replication after primary catch-up.");
checkLog.contains(newPrimary, "Stopping replication producer");

jsTestLog("Enable replication on the new primary so that it can finish state transition");
stopReplProducerOnDocumentFailPoint.off();

assert.eq(replTest.getPrimary(), newPrimary);
stopReplProducerFailPoint.off();
replTest.awaitReplication();

jsTestLog("The transaction has been aborted on the new primary.");
// Create a proxy session to reuse the session state of the old primary.
const newSession = new _DelegatingDriverSession(newPrimary, session);
const newSessionDB = newSession.getDatabase(dbName);
// The transaction has been aborted.
assert.commandFailedWithCode(newSessionDB.adminCommand({
    commitTransaction: 1,
    txnNumber: NumberLong(newSession.getTxnNumber_forTesting()),
    autocommit: false,
    writeConcern: {w: "majority"}
}),
                             ErrorCodes.NoSuchTransaction);

// The old primary rolls back the local committed transaction.
assert.eq(testDB.getCollection(collName).find().itcount(), 0);
assert.eq(newTestDB.getCollection(collName).find().itcount(), 0);

// The transaction table should be the same on both old and new primaries.
txnTableEntry = getTxnTableEntry(newTestDB);
assert.eq(txnTableEntry.state, "aborted");
assert(!txnTableEntry.hasOwnProperty("startOpTime"));
txnTableEntry = getTxnTableEntry(testDB);
assert.eq(txnTableEntry.state, "aborted");
assert(!txnTableEntry.hasOwnProperty("startOpTime"));

jsTestLog("Running another transaction on the new primary");
newSession.startTransaction({writeConcern: {w: 3}});
const secondDoc = {
    _id: "second-doc"
};
assert.commandWorked(newSession.getDatabase(dbName).getCollection(collName).insert(secondDoc));
assert.commandWorked(newSession.commitTransaction_forTesting());
assert.docEq(testDB.getCollection(collName).find().toArray(), [secondDoc]);
assert.docEq(newTestDB.getCollection(collName).find().toArray(), [secondDoc]);

replTest.stopSet();
})();
