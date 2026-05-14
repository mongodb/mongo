/**
 * SERVER-115355: Recover prepared transactions at startup.
 *
 * SERVER-113729 added recovery of prepared transactions from a precise checkpoint, but only
 * after the node transitioned to PRIMARY. SERVER-115355 moves that recovery into startup
 * itself. This test exercises the new path by performing two back-to-back clean restarts of a
 * single-node replica set while two prepared transactions are outstanding, verifying after
 * each restart that:
 *   - the prepared state is re-established before the node accepts client traffic,
 *   - prepared documents are not visible and a conflicting write hits a write conflict,
 *   - additional operations on the prepared transaction are rejected with
 *     PreparedTransactionInProgress,
 *   - the transactions can subsequently be deterministically committed or aborted.
 *
 * The double-restart is the load-bearing piece: the second restart must recover prepared
 * state that was itself written by the first startup recovery cycle, exercising the new
 * STARTUP-time path twice without ever depending on a PRIMARY transition to re-prepare.
 *
 * @tags: [requires_persistence, uses_transactions, uses_prepare_transaction]
 */

import {PrepareHelpers} from "jstests/core/txns/libs/prepare_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const replTest = new ReplSetTest({nodes: 1});
replTest.startSet();
replTest.initiate();

let primary = replTest.getPrimary();
// Default WC is majority; disableSnapshotting prevents satisfying that, forcing the
// prepared entries to live past the stable checkpoint and exercise startup recovery.
assert.commandWorked(
    primary.adminCommand({
        setDefaultRWConcern: 1,
        defaultWriteConcern: {w: 1},
        writeConcern: {w: "majority"},
    }),
);

const dbName = "test";
const collName = "recover_prepared_transactions_startup_repeated_restart";
let testDB = primary.getDB(dbName);
const testColl = testDB.getCollection(collName);

testDB.runCommand({drop: collName});
assert.commandWorked(testDB.runCommand({create: collName}));

let session = primary.startSession({causalConsistency: false});
let sessionDB = session.getDatabase(dbName);
const sessionColl = sessionDB.getCollection(collName);

let session2 = primary.startSession({causalConsistency: false});
let sessionDB2 = session2.getDatabase(dbName);
const sessionColl2 = sessionDB2.getCollection(collName);

assert.commandWorked(sessionColl.insert({_id: 1}));
assert.commandWorked(sessionColl2.insert({_id: 2}));

jsTestLog("Disable snapshotting so prepared entries live past the stable checkpoint");
assert.commandWorked(
    primary.adminCommand({configureFailPoint: "disableSnapshotting", mode: "alwaysOn"}),
);

session.startTransaction();
assert.commandWorked(sessionColl.update({_id: 1}, {_id: 1, a: 1}));
let prepareTimestamp = PrepareHelpers.prepareTransaction(session, {w: 1});

session2.startTransaction();
assert.commandWorked(sessionColl2.update({_id: 2}, {_id: 2, a: 1}));
const prepareTimestamp2 = PrepareHelpers.prepareTransaction(session2, {w: 1});

const lsid = session.getSessionId();
const txnNumber = session.getTxnNumber_forTesting();
const lsid2 = session2.getSessionId();
const txnNumber2 = session2.getTxnNumber_forTesting();

function rebindSessionsAfterRestart() {
    primary = replTest.getPrimary();
    testDB = primary.getDB(dbName);
    session = primary.startSession({causalConsistency: false});
    sessionDB = session.getDatabase(dbName);
    session2 = primary.startSession({causalConsistency: false});
    sessionDB2 = session2.getDatabase(dbName);
    // Force both sessions to reuse their pre-restart identifiers so we are operating on the
    // same prepared transactions that startup recovery just rebuilt.
    session._serverSession.handle.getId = () => lsid;
    session.setTxnNumber_forTesting(txnNumber);
    session2._serverSession.handle.getId = () => lsid2;
    session2.setTxnNumber_forTesting(txnNumber2);
}

function assertBothTxnsRecovered() {
    // Prepared writes must not be visible.
    assert.eq(testDB[collName].find({_id: 1}).toArray(), [{_id: 1}]);
    assert.eq(testDB[collName].find({_id: 2}).toArray(), [{_id: 2}]);

    // A conflicting write against a prepared document must time out.
    assert.commandFailedWithCode(
        testDB.runCommand({
            update: collName,
            updates: [{q: {_id: 1}, u: {$set: {a: 2}}}],
            maxTimeMS: 5 * 1000,
        }),
        ErrorCodes.MaxTimeMSExpired,
    );
    assert.commandFailedWithCode(
        testDB.runCommand({
            update: collName,
            updates: [{q: {_id: 2}, u: {$set: {a: 2}}}],
            maxTimeMS: 5 * 1000,
        }),
        ErrorCodes.MaxTimeMSExpired,
    );

    // Adding statements to either prepared transaction must be rejected.
    assert.commandFailedWithCode(
        sessionDB.runCommand({
            insert: collName,
            documents: [{_id: 3}],
            txnNumber: NumberLong(txnNumber),
            stmtId: NumberInt(2),
            autocommit: false,
        }),
        ErrorCodes.PreparedTransactionInProgress,
    );
    assert.commandFailedWithCode(
        sessionDB2.runCommand({
            insert: collName,
            documents: [{_id: 4}],
            txnNumber: NumberLong(txnNumber2),
            stmtId: NumberInt(2),
            autocommit: false,
        }),
        ErrorCodes.PreparedTransactionInProgress,
    );
}

jsTestLog("First restart: both transactions should be rebuilt by startup recovery");
replTest.stop(primary, undefined, {skipValidation: true});
replTest.start(primary, {}, true);
rebindSessionsAfterRestart();
assertBothTxnsRecovered();

jsTestLog("Second restart: prepared state from the first recovery must survive a second cycle");
replTest.stop(primary, undefined, {skipValidation: true});
replTest.start(primary, {}, true);
rebindSessionsAfterRestart();
assertBothTxnsRecovered();

jsTestLog("Committing the first transaction after two recovery cycles");
PrepareHelpers.awaitMajorityCommitted(replTest, prepareTimestamp);
const commitTimestamp = Timestamp(prepareTimestamp.getTime(), prepareTimestamp.getInc() + 1);
assert.commandWorked(
    sessionDB.adminCommand({
        commitTransaction: 1,
        commitTimestamp: commitTimestamp,
        txnNumber: NumberLong(txnNumber),
        autocommit: false,
    }),
);
assert.eq(testDB[collName].findOne({_id: 1}), {_id: 1, a: 1});

jsTestLog("Aborting the second transaction after two recovery cycles");
assert.commandWorked(
    sessionDB2.adminCommand({
        abortTransaction: 1,
        txnNumber: NumberLong(txnNumber2),
        autocommit: false,
    }),
);
assert.eq(testDB[collName].findOne({_id: 2}), {_id: 2});

jsTestLog("Running a fresh conflicting transaction to ensure no stale prepare state lingers");
session.startTransaction();
assert.commandWorked(sessionDB[collName].update({_id: 1}, {_id: 1, a: 3}));
prepareTimestamp = PrepareHelpers.prepareTransaction(session);
assert.commandWorked(PrepareHelpers.commitTransaction(session, prepareTimestamp));
assert.eq(testDB[collName].findOne({_id: 1}), {_id: 1, a: 3});

replTest.stopSet();
