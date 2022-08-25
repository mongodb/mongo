/**
 * Tests that the work for aborting in-progress transactions on step up is not killable via
 * killSessions commands.
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

const rst = new ReplSetTest({
    name: jsTestName(),
    nodes: 3,
    nodeOptions: {
        setParameter:
            // Make it easier to hold a transaction before it completes.
            {maxNumberOfTransactionOperationsInSingleOplogEntry: 1, bgSyncOplogFetcherBatchSize: 1}
    },
});

rst.startSet();
let config = rst.getReplSetConfig();
config.members[2].priority = 0;
// Disable primary catchup and chaining.
config.settings = {
    catchUpTimeoutMillis: 0,
    chainingAllowed: false
};
rst.initiate(config);

setLogVerbosity(rst.nodes, {"replication": {"verbosity": 3}});

const dbName = "testdb";
const collName = "testcoll";

const primary = rst.nodes[0];
const primaryDB = primary.getDB(dbName);
const newPrimary = rst.nodes[1];
const newPrimaryDB = newPrimary.getDB(dbName);

assert.commandWorked(primaryDB.runCommand({create: collName, writeConcern: {w: "majority"}}));

// Prevent the priority: 0 node from fetching new ops so that it can vote for the new primary.
const stopReplProducerFailPoint = configureFailPoint(rst.nodes[2], 'stopReplProducer');

jsTest.log("Stop secondary oplog replication before the last operation in the transaction.");
// The stopReplProducerOnDocument failpoint ensures that secondary stops replicating before
// applying the last operation in the transaction. This depends on the oplog fetcher batch size
// being 1.
const stopReplProducerOnDocumentFailPoint = configureFailPoint(
    newPrimary, "stopReplProducerOnDocument", {document: {"applyOps.o._id": "last in txn"}});

jsTestLog("Start a transaction.");
const session = primary.startSession({causalConsistency: false});
const sessionDB = session.getDatabase(dbName);
const sessionColl = sessionDB.getCollection(collName);
session.startTransaction({writeConcern: {w: "majority", wtimeout: 500}});

const lsid = session.getSessionId().id;
jsTestLog("LSID for our session is " + tojson(lsid));

jsTestLog("Add inserts to transaction.");
assert.commandWorked(sessionColl.insert({_id: "first in txn on primary " + primary}));
assert.commandWorked(sessionColl.insert({_id: "last in txn"}));

jsTestLog("Confirm we cannot commit the transaction due to insufficient replication.");
let res = session.commitTransaction_forTesting();
assert.commandFailedWithCode(res, ErrorCodes.WriteConcernFailed);

jsTestLog("Find the start and commit optimes on the primary.");
let txnTableEntry = getTxnTableEntry(primaryDB);
assert.eq(txnTableEntry.state, "committed");
const commitOpTime = txnTableEntry.lastWriteOpTime;
const startOpTime =
    primaryDB.getSiblingDB("local").oplog.rs.findOne({ts: commitOpTime.ts}).prevOpTime;

jsTestLog("Wait for the new primary to block on fail point.");
stopReplProducerOnDocumentFailPoint.wait();

jsTestLog("Wait for the new primary to apply the first op of transaction at timestamp: " +
          tojson(startOpTime));
assert.soon(() => {
    const lastOpTime = getLastOpTime(newPrimary);
    jsTestLog("Current lastOpTime on the new primary: " + tojson(lastOpTime));
    return rs.compareOpTimes(lastOpTime, startOpTime) >= 0;
});

// Now the transaction should be in-progress on the new primary.
txnTableEntry = getTxnTableEntry(newPrimaryDB);
assert.eq(txnTableEntry.state, "inProgress");
// The startOpTime should be less than the commit optime.
assert.eq(rs.compareOpTimes(txnTableEntry.startOpTime, commitOpTime), -1);

jsTestLog("Set step up failpoint on new primary");
const stepUpFP = configureFailPoint(newPrimary, "hangDuringStepUpAbortInProgressTransactions");

jsTestLog("Step down primary via heartbeat.");
assert.commandWorked(newPrimary.adminCommand({replSetStepUp: 1}));
rst.awaitNodesAgreeOnPrimary();
reconnect(primary);

jsTestLog("Wait for the new primary to stop replication after primary catch-up.");
checkLog.contains(newPrimary, "Stopping replication producer");

jsTestLog("Enable replication on the new primary so that it can continue the state transition");
stopReplProducerOnDocumentFailPoint.off();

jsTestLog("Wait on new primary to hit step up failpoint");
stepUpFP.wait();

jsTestLog("Attempt to kill the session");
assert.commandWorked(newPrimaryDB.runCommand({killSessions: [{id: lsid}]}));

jsTestLog("Allow step up to continue");
stepUpFP.off();
assert.eq(rst.getPrimary(), newPrimary);
stopReplProducerFailPoint.off();
rst.awaitReplication();

jsTestLog("Verifying that the transaction has been aborted on the new primary.");
// Create a proxy session to reuse the session state of the old primary.
const newSession = new _DelegatingDriverSession(newPrimary, session);
const newSessionDB = newSession.getDatabase(dbName);
// The transaction should have been aborted.
assert.commandFailedWithCode(newSessionDB.adminCommand({
    commitTransaction: 1,
    txnNumber: NumberLong(newSession.getTxnNumber_forTesting()),
    autocommit: false,
    writeConcern: {w: "majority"}
}),
                             ErrorCodes.NoSuchTransaction);

jsTestLog("Verifying that the collection was not changed by the transaction.");
assert.eq(primaryDB.getCollection(collName).find().itcount(), 0);
assert.eq(newPrimaryDB.getCollection(collName).find().itcount(), 0);

rst.stopSet();
})();
