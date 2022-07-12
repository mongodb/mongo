/**
 * Validates TemporarilyUnavailableException handling in query execution. Complements the
 * temporarily_unavailable_error.js noPassthrough test which validates behaviour outside of query
 * execution.
 *
 * @tags: [
 *   # Exclude in-memory engine, rollbacks due to pinned cache content relying on eviction.
 *   requires_journaling,
 *   requires_persistence,
 *   requires_replication,
 *   requires_wiredtiger,
 * ]
 */
(function() {
"use strict";

const maxRetries = 3;

const replSet = new ReplSetTest({
    nodes: 1,
    nodeOptions: {
        wiredTigerCacheSizeGB: 1,
        setParameter: {
            logComponentVerbosity: tojson({control: 1, write: 1}),
            enableTemporarilyUnavailableExceptions: true,
            // Lower these values from the defaults to speed up the test.
            temporarilyUnavailableMaxRetries: maxRetries,
            temporarilyUnavailableBackoffBaseMs: 10,
        }
    },
});
replSet.startSet();
replSet.initiate();
const db = replSet.getPrimary().getDB("test");

// Insert a large document that should pin at least tens of MB of dirty data in WiredTiger. This
// is adapted from the reproducer in the SERVER-61909 ticket description.
assert.commandWorked(db.c.createIndex({x: "text"}));
let doc = {a: 1, x: []};
for (let j = 0; j < 200000; j++) {
    doc.x.push("" + Math.random() + Math.random());
}
assert.commandWorked(db.c.insertOne(doc));

// Shrink the WiredTiger cache so as to reliably get a TemporarilyUnavailableException.
assert.commandWorked(
    db.adminCommand({setParameter: 1, "wiredTigerEngineRuntimeConfig": "cache_size=32M"}));

function temporarilyUnavailableNonTransaction(op) {
    jsTestLog("Temporarily unavailable error on non-transactional " + op);

    const serverStatusBefore = db.serverStatus();

    assert(op === "delete" || op === "update");

    const ret = op === "delete" ? db.c.remove({a: 1}) : db.c.update({a: 1}, {a: 2});

    assert.eq(ret["nRemoved"], 0);
    assert.commandFailedWithCode(ret, ErrorCodes.TemporarilyUnavailable, ret);

    const serverStatusAfter = db.serverStatus();
    // temporarilyUnavailableErrors is incremented by maxRetries + 1, because the last time the
    // exception is not retried.
    assert.gte(serverStatusAfter.metrics.operation.temporarilyUnavailableErrors,
               serverStatusBefore.metrics.operation.temporarilyUnavailableErrors + maxRetries + 1);
    assert.gte(serverStatusAfter.metrics.operation.temporarilyUnavailableErrorsEscaped,
               serverStatusBefore.metrics.operation.temporarilyUnavailableErrorsEscaped + 1);
}

function temporarilyUnavailableInTransactionIsConvertedToWriteConflict(op) {
    jsTestLog("Temporarily unavailable error on transactional " + op);

    assert(op === "delete" || op === "update");

    // Inside a transaction, TemporarilyUnavailable errors should be converted to
    // WriteConflicts and tagged as TransientTransactionErrors.
    const serverStatusBefore = db.serverStatus();
    const session = db.getMongo().startSession();
    session.startTransaction();
    const sessionDB = session.getDatabase("test");
    const ret = op === "delete" ? sessionDB.c.remove({a: 1}) : sessionDB.c.update({a: 1}, {a: 2});

    assert.commandFailedWithCode(ret, ErrorCodes.WriteConflict, ret);
    assert(ret.hasOwnProperty("errorLabels"), ret);
    assert.contains("TransientTransactionError", ret.errorLabels, ret);
    session.abortTransaction();

    const serverStatusAfter = db.serverStatus();
    assert.gt(
        serverStatusAfter.metrics.operation.temporarilyUnavailableErrorsConvertedToWriteConflict,
        serverStatusBefore.metrics.operation.temporarilyUnavailableErrorsConvertedToWriteConflict);
    assert.eq(serverStatusAfter.metrics.operation.temporarilyUnavailableErrors,
              serverStatusBefore.metrics.operation.temporarilyUnavailableErrors);
    assert.eq(serverStatusAfter.metrics.operation.temporarilyUnavailableErrorsEscaped,
              serverStatusBefore.metrics.operation.temporarilyUnavailableErrorsEscaped);
}

for (const op of ["delete", "update"]) {
    temporarilyUnavailableNonTransaction(op);
    temporarilyUnavailableInTransactionIsConvertedToWriteConflict(op);
}

replSet.stopSet();
})();
