/**
 * Test killing 'commitTransaction' and 'abortTransaction' operations on prepared transactions.
 *
 * @tags: [uses_transactions, uses_prepare_transaction]
 */

(function() {
"use strict";
// TODO (SERVER-42987) Re-enable this test.
if (true) {
    return;
}
load("jstests/core/txns/libs/prepare_helpers.js");
load("jstests/libs/parallelTester.js");  // for ScopedThread.

const name = "kill_prepared_transaction_commit_abort";
const rst = new ReplSetTest({
    nodes: 1,
});
rst.startSet();
rst.initiate();

const TxnState = {
    InProgress: 1,
    Committed: 2,
    Aborted: 3,
};

const dbName = "test";
const collName = name;

const primary = rst.getPrimary();
const testDB = primary.getDB(dbName);

// A latch that will act as a signal to shut down the killOp thread.
let shutdownLatch = new CountDownLatch(1);
assert.commandWorked(testDB.runCommand({create: collName}));

/**
 * A function that continuously kills any running 'commitTransaction' or 'abortTransaction' commands
 * on the server, until it receives a shutdown signal via 'shutdownLatch'.
 */
function killOpThread(host, dbName, collName, shutdownLatch) {
    const nodeDB = new Mongo(host).getDB(dbName);
    jsTestLog("killOp thread starting.");
    while (shutdownLatch.getCount() > 0) {
        let filter = {
            "$or": [
                {"command.commitTransaction": 1, active: true},
                {"command.abortTransaction": 1, active: true}
            ]
        };
        let ops = nodeDB.currentOp(filter).inprog;
        if (ops.length > 0) {
            print("Going to run 'killOp' on " + ops.length + " ops.");
        }
        ops.forEach(op => {
            if (op.opid) {
                nodeDB.killOp(op.opid);
            }
        });
    }
    jsTestLog("killOp thread exiting.");
}

/**
 * Creates 'num' sessions and starts and prepares a transaction on each. Returns an array of
 * sessions included with the commit timestamp for each prepared transaction and the current state
 * of that transaction.
 */
function createPreparedTransactions(num) {
    let sessions = [];
    for (let i = 0; i < num; i++) {
        const priSession = primary.startSession();
        const priSessionDB = priSession.getDatabase(dbName);
        const priSessionColl = priSessionDB.getCollection(collName);

        priSession.startTransaction();
        assert.commandWorked(priSessionColl.insert({_id: i}));
        const prepareTimestamp = PrepareHelpers.prepareTransaction(priSession);
        sessions.push(
            {session: priSession, commitTs: prepareTimestamp, state: TxnState.InProgress});
    }
    return sessions;
}

/**
 * Commit or abort transactions on all the given sessions until all transactions are complete. We
 * choose to randomly commit or abort a given transaction with equal probability.
 */
function commitOrAbortAllTransactions(sessions) {
    // Until all transactions have definitively completed, try to abort/commit the open,
    // prepared transactions.
    while (sessions.find(s => (s.state === TxnState.InProgress)) !== undefined) {
        for (let i = 0; i < sessions.length; i++) {
            // We don't need to commit an already completed transaction.
            if (sessions[i].state !== TxnState.InProgress) {
                continue;
            }

            // Randomly choose to commit or abort the transaction.
            let sess = sessions[i];
            let terminalStates = [TxnState.Committed, TxnState.Aborted];
            let terminalState = terminalStates[Math.round(Math.random())];
            let cmd = (terminalState === TxnState.Committed)
                ? {commitTransaction: 1, commitTimestamp: sess.commitTs}
                : {abortTransaction: 1};
            let res = sess.session.getDatabase("admin").adminCommand(cmd);

            if (res.ok === 1) {
                // Mark the transaction's terminal state.
                sessions[i].state = terminalState;
            }
            if (res.ok === 0) {
                // We assume that transaction commit/abort should not fail for any reason other than
                // interruption in this test. If the commit/abort was interrupted, then the command
                // should have failed, and the transaction state should be unaffected.
                assert.commandFailedWithCode(res, ErrorCodes.Interrupted);
                print("Transaction " + i + " was interrupted.");
            }
        }
    }
}

// The number of sessions and transactions to create.
const numTxns = 100;

// Make the server sleep a bit right after commit/abort commands start to make it more likely that
// the kill op thread will be able to find and kill them.
assert.commandWorked(primary.adminCommand({
    configureFailPoint: "sleepMillisAfterCommandExecutionBegins",
    mode: "alwaysOn",
    data: {millis: 50, commands: {"commitTransaction": 1, "abortTransaction": 1}}
}));

jsTestLog("Creating sessions and preparing " + numTxns + " transactions.");
let sessions = createPreparedTransactions(numTxns);

jsTestLog("Starting the killOp thread.");
let killThread = new Thread(killOpThread, primary.host, dbName, collName, shutdownLatch);
killThread.start();

jsTestLog("Committing/aborting all transactions.");
commitOrAbortAllTransactions(sessions);

// Make sure all transactions were completed.
assert(sessions.every(s => (s.state === TxnState.Committed) || (s.state === TxnState.Aborted)));

jsTestLog("Stopping the killOp thread.");
shutdownLatch.countDown();
killThread.join();

jsTestLog("Checking visibility of all transaction operations.");

// If a transaction committed then its document should be visible. If it aborted then its document
// should not be visible.
let docs = testDB[collName].find().toArray();
let committedTxnIds =
    sessions.reduce((acc, s, i) => (s.state === TxnState.Committed ? acc.concat(i) : acc), []);
let commitCount = committedTxnIds.length;
let abortCount = (sessions.length - committedTxnIds.length);
jsTestLog("Committed " + commitCount + " transactions, Aborted " + abortCount + " transactions.");

// Verify that the correct documents exist.
let expectedDocs = committedTxnIds.map((i) => ({_id: i}));
assert.sameMembers(docs, expectedDocs);

// Assert that no prepared transactions are open on any of the sessions we started, and then end
// each session.
for (let i = 0; i < sessions.length; i++) {
    const ops = testDB
                    .currentOp({
                        "lsid.id": sessions[i].session.getSessionId().id,
                        "transaction.timePreparedMicros": {$exists: true}
                    })
                    .inprog;
    assert.eq(ops.length, 0);
    sessions[i].session.endSession();
}

rst.stopSet();
})();
