/**
 * Validate we are aborting multi-document transactions when we are under cache pressure.
 *
 * @tags: [requires_persistence, requires_wiredtiger, featureFlagStorageEngineInterruptibility]
 */

import {PrepareHelpers} from "jstests/core/txns/libs/prepare_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

// Temporarily disable test until the thread is more deterministic
// TODO: SERVER-105782
quit();

// Shrink the WiredTiger cache so we can easily fill it up
let replSet = new ReplSetTest({
    nodes: 1,
    nodeOptions: {wiredTigerEngineConfigString: "cache_size=256M,cache_stuck_timeout_ms=900000"}
});

replSet.startSet();
replSet.initiate();
const db = replSet.getPrimary().getDB("test");

// Ensure the thread is turned on.
assert.commandWorked(db.adminCommand({
    setParameter: 1,
    cachePressureQueryPeriodMilliseconds: 1000,
}));

// Reduce the tracking window so we can complete the test faster.
assert.commandWorked(db.adminCommand({
    setParameter: 1,
    cachePressureEvictionStallDetectionWindowSeconds: 1,
}));

jsTestLog("Starting inserts to force eviction");

// Create a small document to open many sessions quickly.
let doc = {a: 1, x: "a".repeat(500)};

// Create a large document that should pin some dirty data in WiredTiger.
let largeDoc = {a: 1, x: "a".repeat(0.5 * 1024 * 1024)};

assert.commandWorked(db.createCollection("c"));
assert.commandWorked(db.createCollection("d"));

// Have one transaction be prepared to ensure we do not abort this.
let firstPreparedTxn = true;

let sessions = [];
jsTestLog(assert.commandWorked(db.runCommand({isMaster: 1})));

// Insert multiple large documents without committing. Once cache starts
// filling up the request should be forced to start taking part in
// eviction
for (let i = 0; i < 50; i++) {
    let session = db.getMongo().startSession();
    session.startTransaction();
    try {
        assert.commandWorked(
            session.getDatabase("test").runCommand({"insert": "c", documents: [largeDoc]}));
    } catch (e) {
    }
    sessions.push(session);
    jsTestLog("transacting " + sessions.length);

    const abortOldestTransactionsSuccessfulKills =
        db.serverStatus().metrics.abortOldestTransactions.successfulKills;

    // Once we have a successful kill, we know we have aborted the oldest transaction.
    if (abortOldestTransactionsSuccessfulKills > 0) {
        break;
    }
}

// Insert multiple small documents, this should increase the thread pressure.
while (true) {
    let session = db.getMongo().startSession();
    session.startTransaction();
    try {
        assert.commandWorked(
            session.getDatabase("test").runCommand({"insert": "d", documents: [doc]}));

        if (firstPreparedTxn) {
            firstPreparedTxn = false;
            PrepareHelpers.prepareTransaction(session);
        }
    } catch (e) {
    }

    sessions.push(session);
    jsTestLog("transacting " + sessions.length);

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

// Turn off the periodic thread to abort under cache pressure and clean up the test.
assert.commandWorked(db.adminCommand({
    setParameter: 1,
    cachePressureQueryPeriodMilliseconds: 0,
}));

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
