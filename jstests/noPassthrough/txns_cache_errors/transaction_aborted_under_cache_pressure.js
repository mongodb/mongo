/**
 * Validate we are aborting multi-document transactions when we are under cache pressure.
 *
 * @tags: [requires_persistence, requires_wiredtiger, featureFlagStorageEngineInterruptibility]
 */

import {PrepareHelpers} from "jstests/core/txns/libs/prepare_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

// Shrink the WiredTiger cache so we can easily fill it up
let replSet = new ReplSetTest({
    nodes: 1,
    nodeOptions: {wiredTigerEngineConfigString: "cache_size=256M,cache_stuck_timeout_ms=600000"}
});

replSet.startSet();
replSet.initiate();
const db = replSet.getPrimary().getDB("test");

// Ensure the thread is turned on.
assert.commandWorked(db.adminCommand({
    setParameter: 1,
    cachePressureQueryPeriodMilliseconds: 1000,
}));

// Reduce the tracking window so we can detect cache pressure quicker.
assert.commandWorked(db.adminCommand({
    setParameter: 1,
    cachePressureEvictionStallDetectionWindowSeconds: 1,
}));

// Log cache pressure metrics.
function logCacheStatus() {
    const status = db.serverStatus();
    jsTestLog(`Cache pressue wait time threshold exceeded: ${
        status.metrics.cachePressure.waitTimeThresholdExceeded}`);
    jsTestLog(`Cache pressue cache updates threshold exceeded: ${
        status.metrics.cachePressure.cacheUpdatesThreshold}`);
    jsTestLog(`Cache pressue cache dirty threshold exceeded: ${
        status.metrics.cachePressure.cacheDirtyThreshold}`);
}

// Create a large document to pin dirty data in WiredTiger.
let largeDoc = {a: 1, x: "a".repeat(0.5 * 1024 * 1024)};
assert.commandWorked(db.createCollection("c"));

let sessions = [];
let firstPreparedTxn = true;

jsTestLog("Starting large inserts to create cache pressure...");

let abortOldestTransactionsSuccessfulKills = 0;
const timeoutTimestamp = Date.now() + 10 * 60 * 1000;  // 10 minutes timeout

// Insert multiple large documents without committing.
while (true) {
    if (Date.now() > timeoutTimestamp) {
        jsTestLog("Timeout reached, stopping large inserts...");
        break;
    }

    let session = db.getMongo().startSession();
    session.startTransaction();
    try {
        assert.commandWorked(
            session.getDatabase("test").runCommand({"insert": "c", documents: [largeDoc]}));
        if (firstPreparedTxn) {
            firstPreparedTxn = false;
            PrepareHelpers.prepareTransaction(session);
        }
    } catch (e) {
    }
    sessions.push(session);

    jsTestLog(`Large inserts completed: ${sessions.length}`);
    logCacheStatus();

    let status = db.serverStatus();

    abortOldestTransactionsSuccessfulKills =
        db.serverStatus().metrics.abortOldestTransactions.successfulKills;

    // Once we have a successful kill, we know we have aborted the oldest transaction.
    if (abortOldestTransactionsSuccessfulKills > 0) {
        jsTestLog("Oldest transaction successfully aborted under cache pressure.");
        logCacheStatus();
        break;
    }
}

// Check we did not abort the prepared transaction.
let res = assert.commandWorked(db.adminCommand({serverStatus: 1}));
assert.eq(
    res.transactions.totalPreparedThenAborted, 0, "Prepared transaction was aborted unexpectedly");

jsTestLog("Aborting remaining transactions...");

// Abort remaining transactions while handling temporarily unavailable errors
let numTemporarilyUnavailable = 0;
for (let i = 0; i < sessions.length; i++) {
    let res = sessions[i].abortTransaction_forTesting();
    if (res.ok == 0 && res.code == ErrorCodes.TemporarilyUnavailable) {
        numTemporarilyUnavailable++;
    }
}

// At least one of the transactions should return with the temporarily unavailable error code.
assert(numTemporarilyUnavailable > 0, "Expected TemporarilyUnavailable error but none occurred.");
jsTestLog("All transactions aborted as expected.");

replSet.stopSet();
