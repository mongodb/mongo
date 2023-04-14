/**
 * Test ensures that exhausting the number of write tickets in the system does not prevent
 * transactions from being reaped/aborted.
 *
 * @tags: [
 *   requires_fcv_70,
 *   uses_transactions,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/parallelTester.js");  // for Thread

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

            // Setting a transaction lifetime of 1 hour to make sure the transaction reaper
            // doesn't abort the transaction.
            transactionLifetimeLimitSeconds: 3600,
        }
    }
});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const db = primary.getDB("test");

const session = primary.startSession({causalConsistency: false});
const sessionDb = session.getDatabase("test");

assert.commandWorked(db.runCommand({create: "mycoll"}));

session.startTransaction();
assert.commandWorked(sessionDb.mycoll.insert({}));

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

// Attempting to perform another operation inside of the transaction will block and should
// cause it to be aborted implicity.
assert.commandFailedWithCode(sessionDb.mycoll.insert({}), ErrorCodes.LockTimeout);

for (let thread of threads) {
    thread.join();
}

for (let thread of threads) {
    assert.commandWorked(thread.returnData());
}

// Transaction should already be aborted.
let res = assert.commandFailedWithCode(session.abortTransaction_forTesting(),
                                       ErrorCodes.NoSuchTransaction);
assert(res.errmsg.match(/Transaction .* has been aborted/), res.errmsg);

session.endSession();
rst.stopSet();
})();
