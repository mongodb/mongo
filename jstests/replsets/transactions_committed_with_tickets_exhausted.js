/**
 * Test ensures that exhausting the number of write tickets in the system does not prevent
 * transactions from being committed.
 *
 * @tags: [
 *   requires_fcv_70,
 *   uses_transactions,
 *   uses_prepare_transaction,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/parallelTester.js");  // for Thread
load("jstests/core/txns/libs/prepare_helpers.js");

// We set the number of write tickets to be a small value in order to avoid needing to spawn a
// large number of threads to exhaust all of the available ones.
const kNumWriteTickets = 5;

const rst = new ReplSetTest({
    nodes: 1,
    nodeOptions: {
        setParameter: {
            // This test requires a fixed ticket pool size.
            storageEngineConcurrencyAdjustmentAlgorithm: "fixedConcurrentTransactions",
            wiredTigerConcurrentWriteTransactions: kNumWriteTickets,

            // Setting a transaction lifetime of 20 seconds works fine locally because the
            // threads which attempt to run the drop command are spawned quickly enough. This
            // might not be the case for Evergreen hosts and may need to be tuned accordingly.
            transactionLifetimeLimitSeconds: 20,
        }
    }
});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const db = primary.getDB("test");

const session = primary.startSession({causalConsistency: false});
const sessionDb = session.getDatabase("test");

assert.commandWorked(db.runCommand({create: "mycoll", writeConcern: {w: "majority"}}));

jsTestLog("Starting transaction");
session.startTransaction();
assert.commandWorked(sessionDb.mycoll.insert({}));

jsTestLog("Preparing transaction");
let prepareTimestamp = PrepareHelpers.prepareTransaction(session);

const threads = [];

for (let i = 0; i < kNumWriteTickets; ++i) {
    const thread = new Thread(function(host) {
        try {
            const conn = new Mongo(host);
            const db = conn.getDB("test");

            // Dropping a collection requires a database X lock and therefore blocks behind the
            // transaction committing or aborting.
            db.mycoll.drop();

            return {ok: 1};
        } catch (e) {
            return {ok: 0, error: e.toString(), stack: e.stack};
        }
    }, primary.host);

    threads.push(thread);
    thread.start();
}

// We wait until all of the drop commands are waiting for a lock to know that we've exhausted
// all of the available write tickets.
assert.soon(
    () => {
        const ops = db.currentOp({"command.drop": "mycoll", waitingForLock: true});
        return ops.inprog.length === kNumWriteTickets;
    },
    () => {
        return `Didn't find ${kNumWriteTickets} drop commands running: ` + tojson(db.currentOp());
    });

// Should be able to successfully commit the transaction with the write tickets exhausted.
jsTestLog("Committing transaction");
assert.commandWorked(PrepareHelpers.commitTransaction(session, prepareTimestamp));

jsTestLog("Waiting for drop command to join");
for (let thread of threads) {
    thread.join();
}

for (let thread of threads) {
    assert.commandWorked(thread.returnData());
}

rst.stopSet();
})();
