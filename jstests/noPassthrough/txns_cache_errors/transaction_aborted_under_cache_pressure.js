/**
 * Validate we are aborting multi-document transactions when we are under cache pressure.
 *
 * @tags: [requires_persistence, requires_wiredtiger, featureFlagStorageEngineInterruptibility]
 */

import {PrepareHelpers} from "jstests/core/txns/libs/prepare_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

let replSet = new ReplSetTest({nodes: 1});
replSet.startSet();
replSet.initiate();
const db = replSet.getPrimary().getDB("test");

// Shrink the WiredTiger cache so we can easily fill it up
assert.commandWorked(
    db.adminCommand({setParameter: 1, "wiredTigerEngineRuntimeConfig": "cache_size=16M"}));

jsTestLog("Starting inserts to force eviction");

// Create a large document that should pin some dirty data in WiredTiger.
// This is adapted from the reproducer in the SERVER-61909 ticket
// description.
let doc = {a: 1, x: []};
for (let j = 0; j < 200000; j++) {
    doc.x.push("" + Math.random() + Math.random());
}

assert.commandWorked(db.createCollection("c"));

let firstTxn = true;

let sessions = [];
jsTestLog(assert.commandWorked(db.runCommand({isMaster: 1})));
// Insert multiple documents without committing. Once cache starts
// filling up the request should be forced to start taking part in
// eviction
while (true) {
    let session = db.getMongo().startSession();
    session.startTransaction();
    try {
        // Even though after cancelling eviction the command should
        // continue as normal, these can still fail due to another WT
        // feature that kills the oldest running txn
        assert.commandWorked(
            session.getDatabase("test").runCommand({"insert": "c", documents: [doc]}));

        if (firstTxn) {
            firstTxn = false;
            PrepareHelpers.prepareTransaction(session);
        }
    } catch (e) {
    }

    sessions.push(session);

    const abortOldestTransactionsSuccessfulKills =
        db.serverStatus().metrics.abortOldestTransactions.successfulKills;

    // Once we have a successful kill, we know we have aborted the oldest transaction.
    if (abortOldestTransactionsSuccessfulKills > 0) {
        break;
    }
}

// Check we did not abort the prepared transaction.
let res = assert.commandWorked(db.adminCommand({serverStatus: 1}));
assert.eq(res.transactions.totalPreparedThenAborted, 0);

jsTestLog("Aborting remaining transactions");

let numTemporarilyUnavailable = 0;
for (let i = 0; i < sessions.length; i++) {
    let res = sessions[i].abortTransaction_forTesting();
    if (res.ok == 0 && res.code == ErrorCodes.TemporarilyUnavailable) {
        numTemporarilyUnavailable++;
    }
}

// At least one of the transactions should return with the temporarily unavailable error code.
assert(numTemporarilyUnavailable > 0);
jsTestLog("All transactions aborted");

replSet.stopSet();
