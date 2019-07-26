/**
 * Tests that the coordinateCommitTransaction command falls back to recovering the decision from
 * the local participant.
 *
 * TODO (SERVER-37364): Once coordinateCommit returns as soon as the decision is made durable, these
 * tests will pass but will be racy in terms of whether they're testing that coordinateCommit
 * returns the TransactionCoordinator's decision or local TransactionParticipant's decision.
 *
 * @tags: [uses_transactions, uses_prepare_transaction]
 */
(function() {
"use strict";

load("jstests/sharding/libs/sharded_transactions_helpers.js");
load("jstests/libs/write_concern_util.js");

// The test modifies config.transactions, which must be done outside of a session.
TestData.disableImplicitSessions = true;

// Reducing this from the resmoke default, which is several hours, so that tests that rely on a
// transaction coordinator being canceled after a timeout happen in a reasonable amount of time.
TestData.transactionLifetimeLimitSeconds = 15;

const readFromShard0 = function({lsid, txnNumber, startTransaction}) {
    let findDocumentOnShard0Command = {
        find: 'user',
        filter: {x: -1},
        lsid: lsid,
        txnNumber: NumberLong(txnNumber),
        autocommit: false,
    };

    if (startTransaction) {
        findDocumentOnShard0Command.startTransaction = true;
    }

    let res = assert.commandWorked(testDB.runCommand(findDocumentOnShard0Command));
    assert.neq(null, res.recoveryToken);
    return res.recoveryToken;
};

const readFromShard1 = function({lsid, txnNumber, startTransaction}) {
    let findDocumentOnShard1Command = {
        find: 'user',
        filter: {x: 1},
        lsid: lsid,
        txnNumber: NumberLong(txnNumber),
        autocommit: false,
    };

    if (startTransaction) {
        findDocumentOnShard1Command.startTransaction = true;
    }

    let res = assert.commandWorked(testDB.runCommand(findDocumentOnShard1Command));
    assert.neq(null, res.recoveryToken);
    return res.recoveryToken;
};

const writeToShard0 = function({lsid, txnNumber, startTransaction}) {
    const updateDocumentOnShard0 = {
        q: {x: -1},
        u: {"$set": {lastTxnNumber: txnNumber}},
        upsert: true
    };

    let updateDocumentOnShard0Command = {
        update: 'user',
        updates: [updateDocumentOnShard0],
        lsid: lsid,
        txnNumber: NumberLong(txnNumber),
        autocommit: false,
    };

    if (startTransaction) {
        updateDocumentOnShard0Command.startTransaction = true;
    }

    let res = assert.commandWorked(testDB.runCommand(updateDocumentOnShard0Command));
    assert.neq(null, res.recoveryToken);
    return res.recoveryToken;
};

const writeToShard1 = function({lsid, txnNumber, startTransaction}) {
    const updateDocumentOnShard1 = {
        q: {x: 1},
        u: {"$set": {lastTxnNumber: txnNumber}},
        upsert: true
    };

    let updateDocumentOnShard1Command = {
        update: 'user',
        updates: [updateDocumentOnShard1],
        lsid: lsid,
        txnNumber: NumberLong(txnNumber),
        autocommit: false,
    };

    if (startTransaction) {
        updateDocumentOnShard1Command.startTransaction = true;
    }

    let res = assert.commandWorked(testDB.runCommand(updateDocumentOnShard1Command));
    assert.neq(null, res.recoveryToken);
    return res.recoveryToken;
};

const startNewSingleShardReadOnlyTransaction = function() {
    const recoveryToken = readFromShard0({lsid, txnNumber, startTransaction: true});
    assert.eq(null, recoveryToken.recoveryShardId);
    return recoveryToken;
};

const startNewSingleShardWriteTransaction = function() {
    const recoveryToken = writeToShard0({lsid, txnNumber, startTransaction: true});
    assert.neq(null, recoveryToken.recoveryShardId);
    return recoveryToken;
};

const startNewMultiShardReadOnlyTransaction = function() {
    let recoveryToken = readFromShard0({lsid, txnNumber, startTransaction: true});
    assert.eq(null, recoveryToken.recoveryShardId);

    recoveryToken = readFromShard1({lsid, txnNumber});
    assert.eq(null, recoveryToken.recoveryShardId);

    return recoveryToken;
};

const startNewSingleWriteShardTransaction = function() {
    let recoveryToken = readFromShard0({lsid, txnNumber, startTransaction: true});
    assert.eq(null, recoveryToken.recoveryShardId);

    recoveryToken = writeToShard1({lsid, txnNumber});
    assert.neq(null, recoveryToken.recoveryShardId);

    return recoveryToken;
};

const startNewMultiShardWriteTransaction = function() {
    let recoveryToken = readFromShard0({lsid, txnNumber, startTransaction: true});
    assert.eq(null, recoveryToken.recoveryShardId);

    // Write to shard 1, not shard 0, otherwise the recovery shard will still be the same as the
    // coordinator shard.
    recoveryToken = writeToShard1({lsid, txnNumber});
    assert.neq(null, recoveryToken.recoveryShardId);

    recoveryToken = writeToShard0({lsid, txnNumber});
    assert.neq(null, recoveryToken.recoveryShardId);

    return recoveryToken;
};

const abortTransactionOnShardDirectly = function(shardPrimaryConn, lsid, txnNumber) {
    assert.commandWorked(shardPrimaryConn.adminCommand(
        {abortTransaction: 1, lsid: lsid, txnNumber: NumberLong(txnNumber), autocommit: false}));
};

const sendCommitViaOriginalMongos = function(lsid, txnNumber, recoveryToken) {
    return st.s0.getDB('admin').runCommand({
        commitTransaction: 1,
        lsid: lsid,
        txnNumber: NumberLong(txnNumber),
        autocommit: false,
        recoveryToken: recoveryToken
    });
};

const sendCommitViaRecoveryMongos = function(lsid, txnNumber, recoveryToken, writeConcern) {
    writeConcern = writeConcern || {};
    return st.s1.getDB('admin').runCommand(Object.merge({
        commitTransaction: 1,
        lsid: lsid,
        txnNumber: NumberLong(txnNumber),
        autocommit: false,
        recoveryToken: recoveryToken
    },
                                                        writeConcern));
};

let st =
    new ShardingTest({shards: 2, rs: {nodes: 2}, mongos: 2, other: {mongosOptions: {verbose: 3}}});

assert.commandWorked(st.s0.adminCommand({enableSharding: 'test'}));
st.ensurePrimaryShard('test', st.shard0.name);
assert.commandWorked(st.s0.adminCommand({shardCollection: 'test.user', key: {x: 1}}));
assert.commandWorked(st.s0.adminCommand({split: 'test.user', middle: {x: 0}}));
assert.commandWorked(
    st.s0.adminCommand({moveChunk: 'test.user', find: {x: 0}, to: st.shard1.name}));

// Insert documents to prime mongos and shards with the latest sharding metadata.
let testDB = st.s0.getDB('test');
assert.commandWorked(testDB.runCommand({insert: 'user', documents: [{x: -10}, {x: 10}]}));

const lsid = {
    id: UUID()
};
let txnNumber = 0;

//
// Generic test cases that are agnostic as to the transaction type
//

(function() {
jsTest.log("Testing recovering transaction with lower number than latest");
++txnNumber;

const oldTxnNumber = txnNumber;
const oldRecoveryToken = startNewMultiShardWriteTransaction();

txnNumber++;
const newRecoveryToken = startNewMultiShardWriteTransaction();
assert.commandWorked(sendCommitViaOriginalMongos(lsid, txnNumber, newRecoveryToken));

assert.commandFailedWithCode(sendCommitViaRecoveryMongos(lsid, oldTxnNumber, oldRecoveryToken),
                             ErrorCodes.TransactionTooOld);

// The client can still the recover decision for current transaction number.
assert.commandWorked(sendCommitViaRecoveryMongos(lsid, txnNumber, newRecoveryToken));
})();

(function() {
jsTest.log("Testing recovering transaction with higher number than latest");
txnNumber++;

const oldTxnNumber = txnNumber;
const oldRecoveryToken = startNewMultiShardWriteTransaction();

txnNumber++;
const fakeRecoveryToken = {
    recoveryShardId: st.shard0.shardName
};
assert.commandFailedWithCode(sendCommitViaRecoveryMongos(lsid, txnNumber, fakeRecoveryToken),
                             ErrorCodes.NoSuchTransaction);

// The active transaction can still be committed.
assert.commandWorked(sendCommitViaOriginalMongos(lsid, oldTxnNumber, oldRecoveryToken));
})();

(function() {
jsTest.log("Testing recovering transaction whose recovery shard forgot the transaction");
txnNumber++;

const recoveryToken = startNewMultiShardWriteTransaction();
assert.commandWorked(sendCommitViaOriginalMongos(lsid, txnNumber, recoveryToken));

assert.writeOK(st.rs1.getPrimary().getDB("config").transactions.remove({}, false /* justOne */));

assert.commandFailedWithCode(sendCommitViaRecoveryMongos(lsid, txnNumber, recoveryToken),
                             ErrorCodes.NoSuchTransaction);
})();

(function() {
jsTest.log("Testing that a recovery node does a noop write before returning 'aborted'");

txnNumber++;

const recoveryToken = startNewMultiShardWriteTransaction();
abortTransactionOnShardDirectly(st.rs0.getPrimary(), lsid, txnNumber);
assert.commandFailedWithCode(sendCommitViaOriginalMongos(lsid, txnNumber, recoveryToken),
                             ErrorCodes.NoSuchTransaction);

const recoveryShardReplSetTest = st.rs1;

stopReplicationOnSecondaries(recoveryShardReplSetTest);

// Do a write on the recovery node to bump the recovery node's system last OpTime.
recoveryShardReplSetTest.getPrimary().getDB("dummy").getCollection("dummy").insert({dummy: 1});

// While the recovery shard primary cannot majority commit writes, commitTransaction returns
// NoSuchTransaction with a writeConcern error.
let res = sendCommitViaRecoveryMongos(
    lsid, txnNumber, recoveryToken, {writeConcern: {w: "majority", wtimeout: 500}});
assert.commandFailedWithCode(res, ErrorCodes.NoSuchTransaction);
checkWriteConcernTimedOut(res);

// Once the recovery shard primary can majority commit writes again, commitTransaction
// returns NoSuchTransaction without a writeConcern error.
restartReplicationOnSecondaries(recoveryShardReplSetTest);
res = sendCommitViaRecoveryMongos(lsid, txnNumber, recoveryToken, {writeConcern: {w: "majority"}});
assert.commandFailedWithCode(res, ErrorCodes.NoSuchTransaction);
assert.eq(null, res.writeConcernError);
})();

(function() {
jsTest.log("Testing that a recovery node does a noop write before returning 'committed'");

txnNumber++;

const recoveryToken = startNewMultiShardWriteTransaction();
assert.commandWorked(sendCommitViaOriginalMongos(lsid, txnNumber, recoveryToken));

const recoveryShardReplSetTest = st.rs1;

stopReplicationOnSecondaries(recoveryShardReplSetTest);

// Do a write on the recovery node to bump the recovery node's system last OpTime.
recoveryShardReplSetTest.getPrimary().getDB("dummy").getCollection("dummy").insert({dummy: 1});

// While the recovery shard primary cannot majority commit writes, commitTransaction returns
// ok with a writeConcern error.
let res = sendCommitViaRecoveryMongos(
    lsid, txnNumber, recoveryToken, {writeConcern: {w: "majority", wtimeout: 500}});
assert.commandWorkedIgnoringWriteConcernErrors(res);
checkWriteConcernTimedOut(res);

// Once the recovery shard primary can majority commit writes again, commitTransaction
// returns ok without a writeConcern error.
restartReplicationOnSecondaries(recoveryShardReplSetTest);
assert.commandWorked(
    sendCommitViaRecoveryMongos(lsid, txnNumber, recoveryToken, {writeConcern: {w: "majority"}}));
})();

//
// Single-shard read-only transactions
//

(function() {
jsTest.log("Testing recovering single-shard read-only transaction that is in progress");
txnNumber++;
const recoveryToken = startNewSingleShardReadOnlyTransaction();
assert.commandFailedWithCode(sendCommitViaRecoveryMongos(lsid, txnNumber, recoveryToken),
                             ErrorCodes.NoSuchTransaction);

// A read-only transaction can still commit after reporting an abort decision.
assert.commandWorked(sendCommitViaOriginalMongos(lsid, txnNumber, recoveryToken));
}());

(function() {
jsTest.log("Testing recovering single-shard read-only transaction that aborted");
txnNumber++;
const recoveryToken = startNewSingleShardReadOnlyTransaction();
abortTransactionOnShardDirectly(st.rs0.getPrimary(), lsid, txnNumber);
assert.commandFailedWithCode(sendCommitViaRecoveryMongos(lsid, txnNumber, recoveryToken),
                             ErrorCodes.NoSuchTransaction);
}());

(function() {
jsTest.log("Testing recovering single-shard read-only transaction that committed");
txnNumber++;
const recoveryToken = startNewSingleShardReadOnlyTransaction();
assert.commandWorked(sendCommitViaOriginalMongos(lsid, txnNumber, recoveryToken));
assert.commandFailedWithCode(sendCommitViaRecoveryMongos(lsid, txnNumber, recoveryToken),
                             ErrorCodes.NoSuchTransaction);
}());

//
// Single-shard write transactions
//

(function() {
jsTest.log("Testing recovering single-shard write transaction that in progress");
txnNumber++;
const recoveryToken = startNewSingleShardWriteTransaction();
assert.commandFailedWithCode(sendCommitViaRecoveryMongos(lsid, txnNumber, recoveryToken),
                             ErrorCodes.NoSuchTransaction);

// A write transaction fails to commit after having reported an abort decision.
assert.commandFailedWithCode(sendCommitViaOriginalMongos(lsid, txnNumber, recoveryToken),
                             ErrorCodes.NoSuchTransaction);
}());

(function() {
jsTest.log("Testing recovering single-shard write transaction that aborted");
txnNumber++;
const recoveryToken = startNewSingleShardWriteTransaction();
abortTransactionOnShardDirectly(st.rs0.getPrimary(), lsid, txnNumber);
assert.commandFailedWithCode(sendCommitViaRecoveryMongos(lsid, txnNumber, recoveryToken),
                             ErrorCodes.NoSuchTransaction);
}());

(function() {
jsTest.log("Testing recovering single-shard write transaction that committed");
txnNumber++;
const recoveryToken = startNewSingleShardWriteTransaction();
assert.commandWorked(sendCommitViaOriginalMongos(lsid, txnNumber, recoveryToken));
assert.commandWorked(sendCommitViaRecoveryMongos(lsid, txnNumber, recoveryToken));
}());

//
// Multi-shard read-only transactions
//

(function() {
jsTest.log("Testing recovering multi-shard read-only transaction that is in progress");
txnNumber++;
const recoveryToken = startNewMultiShardReadOnlyTransaction();

assert.commandFailedWithCode(sendCommitViaRecoveryMongos(lsid, txnNumber, recoveryToken),
                             ErrorCodes.NoSuchTransaction);

// A read-only transaction can still commit after reporting an abort decision.
assert.commandWorked(sendCommitViaOriginalMongos(lsid, txnNumber, recoveryToken));
})();

(function() {
jsTest.log("Testing recovering multi-shard read-only transaction that aborted");
txnNumber++;
const recoveryToken = startNewMultiShardReadOnlyTransaction();
abortTransactionOnShardDirectly(st.rs0.getPrimary(), lsid, txnNumber);
assert.commandFailedWithCode(sendCommitViaRecoveryMongos(lsid, txnNumber, recoveryToken),
                             ErrorCodes.NoSuchTransaction);
})();

(function() {
jsTest.log("Testing recovering multi-shard read-only transaction that committed");
txnNumber++;
const recoveryToken = startNewMultiShardReadOnlyTransaction();
assert.commandWorked(sendCommitViaOriginalMongos(lsid, txnNumber, recoveryToken));
assert.commandFailedWithCode(sendCommitViaRecoveryMongos(lsid, txnNumber, recoveryToken),
                             ErrorCodes.NoSuchTransaction);
})();

//
// Single-write-shard transactions (there are multiple participants but only one did a write)
//

(function() {
jsTest.log("Testing recovering single-write-shard transaction that is in progress");
txnNumber++;
const recoveryToken = startNewSingleWriteShardTransaction();
assert.commandFailedWithCode(sendCommitViaRecoveryMongos(lsid, txnNumber, recoveryToken),
                             ErrorCodes.NoSuchTransaction);

// A write transaction fails to commit after having reported an abort decision.
assert.commandFailedWithCode(sendCommitViaOriginalMongos(lsid, txnNumber, recoveryToken),
                             ErrorCodes.NoSuchTransaction);
}());

(function() {
jsTest.log("Testing recovering single-write-shard transaction that aborted on read-only shard" +
           " but is in progress on write shard");
txnNumber++;
const recoveryToken = startNewSingleWriteShardTransaction();
abortTransactionOnShardDirectly(st.rs1.getPrimary(), lsid, txnNumber);
assert.commandFailedWithCode(sendCommitViaRecoveryMongos(lsid, txnNumber, recoveryToken),
                             ErrorCodes.NoSuchTransaction);
}());

(function() {
jsTest.log("Testing recovering single-write-shard transaction that aborted on write" +
           " shard but is in progress on read-only shard");
txnNumber++;
const recoveryToken = startNewSingleWriteShardTransaction();
abortTransactionOnShardDirectly(st.rs1.getPrimary(), lsid, txnNumber);
assert.commandFailedWithCode(sendCommitViaRecoveryMongos(lsid, txnNumber, recoveryToken),
                             ErrorCodes.NoSuchTransaction);
}());

(function() {
jsTest.log("Testing recovering single-write-shard transaction that committed");
txnNumber++;
const recoveryToken = startNewSingleWriteShardTransaction();
assert.commandWorked(sendCommitViaOriginalMongos(lsid, txnNumber, recoveryToken));
assert.commandWorked(sendCommitViaRecoveryMongos(lsid, txnNumber, recoveryToken));
}());

//
// Multi-write-shard transactions (there are multiple participants and more than one did writes)
//

(function() {
jsTest.log("Testing recovering multi-write-shard transaction that is in progress");
txnNumber++;

// Set the transaction expiry to be very high, so we can ascertain the recovery request
// through the alternate router is what causes the transaction to abort.
const getParamRes =
    st.rs1.getPrimary().adminCommand({getParameter: 1, transactionLifetimeLimitSeconds: 1});
assert.commandWorked(getParamRes);
assert.neq(null, getParamRes.transactionLifetimeLimitSeconds);
const originalTransactionLifetimeLimitSeconds = getParamRes.transactionLifetimeLimitSeconds;

assert.commandWorked(st.rs1.getPrimary().adminCommand(
    {setParameter: 1, transactionLifetimeLimitSeconds: 60 * 60 * 1000 /* 1000 hours */}));

const recoveryToken = startNewMultiShardWriteTransaction();
assert.commandFailedWithCode(sendCommitViaRecoveryMongos(lsid, txnNumber, recoveryToken),
                             ErrorCodes.NoSuchTransaction);

// A write transaction fails to commit after having reported an abort decision.
assert.commandFailedWithCode(sendCommitViaOriginalMongos(lsid, txnNumber, recoveryToken),
                             ErrorCodes.NoSuchTransaction);

assert.commandWorked(st.rs1.getPrimary().adminCommand(
    {setParameter: 1, transactionLifetimeLimitSeconds: originalTransactionLifetimeLimitSeconds}));
})();

(function() {
jsTest.log("Testing recovering multi-write-shard transaction that is in prepare");
txnNumber++;
const recoveryToken = startNewMultiShardWriteTransaction();

// Ensure the coordinator will hang after putting the participants into prepare but
// before sending the decision to the participants.
clearRawMongoProgramOutput();
assert.commandWorked(st.rs0.getPrimary().adminCommand(
    {configureFailPoint: "hangBeforeWritingDecision", mode: "alwaysOn"}));

assert.commandFailedWithCode(st.s0.adminCommand({
    commitTransaction: 1,
    lsid: lsid,
    txnNumber: NumberLong(txnNumber),
    autocommit: false,
    // Specify maxTimeMS to make the command return so the test can continue.
    maxTimeMS: 3000,
}),
                             ErrorCodes.MaxTimeMSExpired);

waitForFailpoint("Hit hangBeforeWritingDecision failpoint", 1);

// Trying to recover the decision should block because the recovery shard's participant
// is in prepare.
assert.commandFailedWithCode(st.s1.adminCommand({
    commitTransaction: 1,
    lsid: lsid,
    txnNumber: NumberLong(txnNumber),
    autocommit: false,
    recoveryToken: recoveryToken,
    // Specify maxTimeMS to make the command return so the test can continue.
    maxTimeMS: 3000,
}),
                             ErrorCodes.MaxTimeMSExpired);

// Allow the transaction to complete.
assert.commandWorked(st.rs0.getPrimary().adminCommand(
    {configureFailPoint: "hangBeforeWritingDecision", mode: "off"}));

// Trying to recover the decision should now return that the transaction committed.
assert.commandWorked(sendCommitViaRecoveryMongos(lsid, txnNumber, recoveryToken));
})();

(function() {
jsTest.log("Testing recovering multi-write-shard transaction after coordinator finished" +
           " coordinating an abort decision.");
txnNumber++;

const recoveryToken = startNewMultiShardWriteTransaction();
abortTransactionOnShardDirectly(st.rs0.getPrimary(), lsid, txnNumber);
assert.commandFailedWithCode(sendCommitViaOriginalMongos(lsid, txnNumber, recoveryToken),
                             ErrorCodes.NoSuchTransaction);
assert.commandFailedWithCode(sendCommitViaRecoveryMongos(lsid, txnNumber, recoveryToken),
                             ErrorCodes.NoSuchTransaction);
})();

(function() {
jsTest.log("Testing recovering multi-write-shard transaction after coordinator finished" +
           " coordinating a commit decision.");
txnNumber++;

const recoveryToken = startNewMultiShardWriteTransaction();
assert.commandWorked(sendCommitViaOriginalMongos(lsid, txnNumber, recoveryToken));
assert.commandWorked(sendCommitViaRecoveryMongos(lsid, txnNumber, recoveryToken));
})();

st.stop();
})();
