/**
 * Validate that under significant WiredTiger cache pressure an operation can fail
 * with TemporarilyUnavailable error.
 *
 * @tags: [
 *   # Exclude in-memory engine, rollbacks due to pinned cache content rely on eviction.
 *   requires_persistence,
 *   requires_replication,
 *   requires_wiredtiger,
 * ]
 */
(function() {
"use strict";

const replSet = new ReplSetTest({
    nodes: 1,
    nodeOptions: {
        wiredTigerCacheSizeGB: 0.256,
        setParameter: {
            logComponentVerbosity: tojson({control: 1, write: 1}),
            enableTemporarilyUnavailableExceptions: true,
            // Lower these values from the defaults to speed up the test.
            temporarilyUnavailableMaxRetries: 3,
            temporarilyUnavailableBackoffBaseMs: 10,
        }
    },
});
replSet.startSet();
replSet.initiate();
const db = replSet.getPrimary().getDB("test");

// Generate a workload pinning enough dirty data in cache that causes WiredTiger
// to roll back transactions. This workload is adapted from the reproducer in the
// SERVER-61909 ticket description.
assert.commandWorked(db.c.createIndex({x: "text"}));
let doc = {x: []};
for (let j = 0; j < 50000; j++)
    doc.x.push("" + Math.random() + Math.random());

(function temporarilyUnavailableNonTransaction() {
    const serverStatusBefore = db.serverStatus();
    let caughtTUerror = false;
    let attempts;
    for (attempts = 1; attempts <= 20; attempts++) {
        print("temporarilyUnavailableNonTransaction attempt " + attempts);
        const ret = db.c.insert(doc);

        if (ret["nInserted"] === 1) {
            // The write succeeded.
            continue;
        }
        assert.eq(0, ret["nInserted"]);
        assert.commandFailedWithCode(ret, ErrorCodes.TemporarilyUnavailable);
        caughtTUerror = true;
        jsTestLog("returned the expected TemporarilyUnavailable code at attempt " + attempts);
        break;
    }

    assert(caughtTUerror,
           "did not return the expected TemporarilyUnavailable error after " + (attempts - 1) +
               " attempts");
    const serverStatusAfter = db.serverStatus();
    assert.gt(serverStatusAfter.metrics.operation.temporarilyUnavailableErrors,
              serverStatusBefore.metrics.operation.temporarilyUnavailableErrors);
    assert.gt(serverStatusAfter.metrics.operation.temporarilyUnavailableErrorsEscaped,
              serverStatusBefore.metrics.operation.temporarilyUnavailableErrorsEscaped);
})();

(function temporarilyUnavailableInTransactionIsConvertedToWriteConflict() {
    // Inside a transaction, TemporarilyUnavailable errors should be converted to
    // WriteConflicts and tagged as TransientTransactionErrors.
    const serverStatusBefore = db.serverStatus();
    let caughtWriteConflict = false;
    let attempts;
    let ret;
    for (attempts = 1; attempts <= 20; attempts++) {
        print("temporarilyUnavailableInTransactionIsConvertedToWriteConflict attempt " + attempts);
        const session = db.getMongo().startSession();
        session.startTransaction();
        const sessionDB = session.getDatabase("test");
        ret = sessionDB.c.insert(doc);

        if (ret["nInserted"] === 1) {
            // The write succeeded.
            session.commitTransaction();
            continue;
        }
        assert.commandFailedWithCode(ret, ErrorCodes.WriteConflict, ret);
        assert(ret.hasOwnProperty("errorLabels"), ret);
        assert.contains("TransientTransactionError", ret.errorLabels, ret);
        caughtWriteConflict = true;
        jsTestLog("returned the expected WriteConflict code at attempt " + attempts);
        session.abortTransaction();
        break;
    }

    assert(caughtWriteConflict,
           "did not return the expected WriteConflict error after " + (attempts - 1) +
               " attempts. Result: " + tojson(ret));

    const serverStatusAfter = db.serverStatus();
    assert.gt(
        serverStatusAfter.metrics.operation.temporarilyUnavailableErrorsConvertedToWriteConflict,
        serverStatusBefore.metrics.operation.temporarilyUnavailableErrorsConvertedToWriteConflict);
    assert.eq(serverStatusAfter.metrics.operation.temporarilyUnavailableErrors,
              serverStatusBefore.metrics.operation.temporarilyUnavailableErrors);
    assert.eq(serverStatusAfter.metrics.operation.temporarilyUnavailableErrorsEscaped,
              serverStatusBefore.metrics.operation.temporarilyUnavailableErrorsEscaped);
})();

replSet.stopSet();
})();
