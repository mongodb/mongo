// Tests multi-statement transactions metrics in the serverStatus output from mongos in various
// basic cases.
// @tags: [
//   uses_multi_shard_transaction,
//   uses_transactions,
// ]
(function() {
"use strict";

load("jstests/libs/curop_helpers.js");   // For waitForCurOpByFailPoint().
load("jstests/libs/parallelTester.js");  // for Thread.
load("jstests/sharding/libs/sharded_transactions_helpers.js");

// Verifies the transaction server status response has the fields that we expect.
function verifyServerStatusFields(res) {
    const expectedFields = [
        "currentOpen",
        "currentActive",
        "currentInactive",
        "totalStarted",
        "totalAborted",
        "abortCause",
        "totalCommitted",
        "totalContactedParticipants",
        "totalParticipantsAtCommit",
        "totalRequestsTargeted",
        "commitTypes",
    ];

    assert(res.hasOwnProperty("transactions"),
           "Expected serverStatus response to have a 'transactions' field, res: " + tojson(res));

    assert.hasFields(res.transactions,
                     expectedFields,
                     "The 'transactions' field did not have all of the expected fields, res: " +
                         tojson(res.transactions));

    assert.eq(expectedFields.length,
              Object.keys(res.transactions).length,
              "the 'transactions' field had an unexpected number of fields, res: " +
                  tojson(res.transactions));

    // Verify the "commitTypes" sub-object has the expected fields.
    const commitTypes = [
        "noShards",
        "singleShard",
        "singleWriteShard",
        "readOnly",
        "twoPhaseCommit",
        "recoverWithToken",
    ];
    const commitTypeFields = ["initiated", "successful", "successfulDurationMicros"];

    assert.hasFields(res.transactions.commitTypes,
                     commitTypes,
                     "The 'transactions' field did not have each expected commit type, res: " +
                         tojson(res.transactions));

    assert.eq(commitTypes.length,
              Object.keys(res.transactions.commitTypes).length,
              "the 'transactions' field had an unexpected number of commit types, res: " +
                  tojson(res.transactions));

    commitTypes.forEach((type) => {
        assert.hasFields(res.transactions.commitTypes[type],
                         commitTypeFields,
                         "commit type " + type +
                             " did not have all the expected fields, commit types: " +
                             tojson(res.transactions.commitTypes));

        assert.eq(commitTypeFields.length,
                  Object.keys(res.transactions.commitTypes[type]).length,
                  "commit type " + type + " had an unexpected number of fields, commit types: " +
                      tojson(res.transactions.commitTypes));
    });
}

class ExpectedCommitType {
    constructor() {
        this.initiated = 0;
        this.successful = 0;
        this.successfulDurationMicros = 0;
    }
}

class ExpectedAbortCause {
    constructor() {
    }
}

class ExpectedTransactionServerStatus {
    constructor() {
        this.currentOpen = 0;
        this.currentActive = 0;
        this.currentInactive = 0;
        this.totalStarted = 0;
        this.totalAborted = 0;
        this.abortCause = new ExpectedAbortCause();
        this.totalCommitted = 0;
        this.totalContactedParticipants = 0;
        this.totalParticipantsAtCommit = 0;
        this.totalRequestsTargeted = 0;
        this.commitTypes = {
            noShards: new ExpectedCommitType(),
            singleShard: new ExpectedCommitType(),
            singleWriteShard: new ExpectedCommitType(),
            readOnly: new ExpectedCommitType(),
            twoPhaseCommit: new ExpectedCommitType(),
            recoverWithToken: new ExpectedCommitType(),
        };
    }
}

// Verifies the transaction values in the server status response match the provided values.
function verifyServerStatusValues(st, expectedStats) {
    const res = assert.commandWorked(st.s.adminCommand({serverStatus: 1}));
    verifyServerStatusFields(res);

    const stats = res.transactions;
    assert.eq(expectedStats.currentOpen,
              stats.currentOpen,
              "unexpected currentOpen, res: " + tojson(stats));
    assert.eq(expectedStats.currentActive,
              stats.currentActive,
              "unexpected currentActive, res: " + tojson(stats));
    assert.eq(expectedStats.currentInactive,
              stats.currentInactive,
              "unexpected currentInactive, res: " + tojson(stats));
    assert.eq(expectedStats.totalStarted,
              stats.totalStarted,
              "unexpected totalStarted, res: " + tojson(stats));
    assert.eq(expectedStats.totalAborted,
              stats.totalAborted,
              "unexpected totalAborted, res: " + tojson(stats));
    assert.eq(expectedStats.totalCommitted,
              stats.totalCommitted,
              "unexpected totalCommitted, res: " + tojson(stats));
    assert.eq(expectedStats.totalContactedParticipants,
              stats.totalContactedParticipants,
              "unexpected totalContactedParticipants, res: " + tojson(stats));
    assert.eq(expectedStats.totalParticipantsAtCommit,
              stats.totalParticipantsAtCommit,
              "unexpected totalParticipantsAtCommit, res: " + tojson(stats));
    assert.eq(expectedStats.totalRequestsTargeted,
              stats.totalRequestsTargeted,
              "unexpected totalRequestsTargeted, res: " + tojson(stats));

    const commitTypes = res.transactions.commitTypes;
    const noShardsCommit = "noShards";
    Object.keys(commitTypes).forEach((commitType) => {
        assert.eq(
            expectedStats.commitTypes[commitType].initiated,
            commitTypes[commitType].initiated,
            "unexpected initiated for " + commitType + ", commit types: " + tojson(commitTypes));
        assert.eq(
            expectedStats.commitTypes[commitType].successful,
            commitTypes[commitType].successful,
            "unexpected successful for " + commitType + ", commit types: " + tojson(commitTypes));

        assert.lte(expectedStats.commitTypes[commitType].successfulDurationMicros,
                   commitTypes[commitType].successfulDurationMicros,
                   "unexpected successfulDurationMicros for " + commitType +
                       ", commit types: " + tojson(commitTypes));
        expectedStats.commitTypes[commitType].successfulDurationMicros =
            commitTypes[commitType].successfulDurationMicros;

        if (commitTypes[commitType].successful != 0 && commitType !== noShardsCommit) {
            assert.gt(commitTypes[commitType].successfulDurationMicros,
                      0,
                      "unexpected successfulDurationMicros for " + commitType +
                          ", commit types: " + tojson(commitTypes));
        }
    });

    const abortCause = res.transactions.abortCause;
    Object.keys(abortCause).forEach((cause) => {
        assert.eq(expectedStats.abortCause[cause],
                  abortCause[cause],
                  "unexpected abortCause for " + cause + ", res: " + tojson(stats));
    });

    assert.eq(
        Object.keys(abortCause).length,
        Object.keys(expectedStats.abortCause).length,
        "the 'transactions' field had an unexpected number of abort causes, res: " + tojson(stats));
}

function abortFromUnderneath(st, session) {
    st._rs.forEach((rs) => {
        assert.commandWorkedOrFailedWithCode(rs.test.getPrimary().adminCommand({
            abortTransaction: 1,
            lsid: session.getSessionId(),
            txnNumber: session.getTxnNumber_forTesting(),
            autocommit: false
        }),
                                             ErrorCodes.NoSuchTransaction);
    });
}

const dbName = "test";
const collName = "foo";
const ns = dbName + '.' + collName;

const st = new ShardingTest({
    shards: 2,
    mongos: 2,
    config: 1,
    other: {
        mongosOptions:
            {setParameter: {'failpoint.skipClusterParameterRefresh': "{'mode':'alwaysOn'}"}}
    }
});

const session = st.s.startSession();
const sessionDB = session.getDatabase(dbName);

const otherRouterSession = st.s1.startSession();
const otherRouterSessionDB = otherRouterSession.getDatabase(dbName);

// Set up two chunks: [-inf, 0), [0, inf) one on each shard, with one document in each.

assert.commandWorked(sessionDB[collName].createIndex({skey: 1}));
assert.commandWorked(sessionDB[collName].insert({skey: -1}));
assert.commandWorked(sessionDB[collName].insert({skey: 1}));

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
st.ensurePrimaryShard(dbName, st.shard0.shardName);
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {skey: 1}}));
assert.commandWorked(st.s.adminCommand({split: ns, middle: {skey: 0}}));
assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {skey: 1}, to: st.shard1.shardName}));
flushRoutersAndRefreshShardMetadata(st, {ns});

let expectedStats = new ExpectedTransactionServerStatus();

let nextSkey = 0;
const uniqueSkey = function uniqueSkey() {
    return nextSkey++;
};

//
// Helpers for setting up transactions that will trigger the various commit paths.
//

function startNoShardsTransaction() {
    session.startTransaction();
    assert.commandWorked(session.getDatabase("doesntExist").runCommand({find: collName}));

    expectedStats.currentOpen += 1;
    expectedStats.currentInactive += 1;
    expectedStats.totalStarted += 1;
    verifyServerStatusValues(st, expectedStats);
}

function startSingleShardTransaction() {
    session.startTransaction();
    assert.commandWorked(sessionDB[collName].insert({skey: uniqueSkey(), x: 1}));

    expectedStats.currentOpen += 1;
    expectedStats.currentInactive += 1;
    expectedStats.totalStarted += 1;
    expectedStats.totalContactedParticipants += 1;
    expectedStats.totalRequestsTargeted += 1;
    verifyServerStatusValues(st, expectedStats);
}

function startSingleWriteShardTransaction() {
    session.startTransaction();
    assert.commandWorked(sessionDB[collName].insert({skey: uniqueSkey(), x: 1}));

    expectedStats.currentOpen += 1;
    expectedStats.currentInactive += 1;
    expectedStats.totalStarted += 1;
    expectedStats.totalContactedParticipants += 1;
    expectedStats.totalRequestsTargeted += 1;
    verifyServerStatusValues(st, expectedStats);

    assert.commandWorked(sessionDB.runCommand({find: collName}));

    expectedStats.totalContactedParticipants += 1;
    expectedStats.totalRequestsTargeted += 2;
    verifyServerStatusValues(st, expectedStats);
}

function startReadOnlyTransaction() {
    session.startTransaction();
    assert.commandWorked(sessionDB.runCommand({find: collName}));

    expectedStats.currentOpen += 1;
    expectedStats.currentInactive += 1;
    expectedStats.totalStarted += 1;
    expectedStats.totalContactedParticipants += 2;
    expectedStats.totalRequestsTargeted += 2;
    verifyServerStatusValues(st, expectedStats);
}

function startTwoPhaseCommitTransaction() {
    session.startTransaction();
    assert.commandWorked(sessionDB[collName].insert({skey: -5}));

    expectedStats.currentOpen += 1;
    expectedStats.currentInactive += 1;
    expectedStats.totalStarted += 1;
    expectedStats.totalContactedParticipants += 1;
    expectedStats.totalRequestsTargeted += 1;
    verifyServerStatusValues(st, expectedStats);

    assert.commandWorked(sessionDB[collName].insert({skey: 5}));

    expectedStats.totalContactedParticipants += 1;
    expectedStats.totalRequestsTargeted += 1;
    verifyServerStatusValues(st, expectedStats);
}

function setUpTransactionToRecoverCommit({shouldCommit}) {
    otherRouterSession.startTransaction();
    let resWithRecoveryToken = assert.commandWorked(otherRouterSessionDB.runCommand(
        {insert: collName, documents: [{skey: uniqueSkey(), x: 5}]}));
    if (shouldCommit) {
        assert.commandWorked(otherRouterSession.commitTransaction_forTesting());
    } else {
        assert.commandWorked(otherRouterSession.abortTransaction_forTesting());
    }

    // The stats on the main mongos shouldn't have changed.
    verifyServerStatusValues(st, expectedStats);

    return resWithRecoveryToken.recoveryToken;
}

//
// Test cases for serverStatus output.
//

jsTest.log("Default values.");
(() => {
    verifyServerStatusValues(st, expectedStats);
})();

// Note committing a no shards transaction can only succeed.
jsTest.log("Committed no shards transaction.");
(() => {
    startNoShardsTransaction();

    assert.commandWorked(session.commitTransaction_forTesting());

    expectedStats.currentOpen -= 1;
    expectedStats.currentInactive -= 1;
    expectedStats.totalCommitted += 1;
    expectedStats.commitTypes.noShards.initiated += 1;
    expectedStats.commitTypes.noShards.successful += 1;
    verifyServerStatusValues(st, expectedStats);
})();

jsTest.log("Successful single shard transaction.");
(() => {
    startSingleShardTransaction();

    assert.commandWorked(session.commitTransaction_forTesting());

    expectedStats.currentOpen -= 1;
    expectedStats.currentInactive -= 1;
    expectedStats.totalCommitted += 1;
    expectedStats.commitTypes.singleShard.initiated += 1;
    expectedStats.commitTypes.singleShard.successful += 1;
    expectedStats.totalParticipantsAtCommit += 1;
    expectedStats.totalRequestsTargeted += 1;
    verifyServerStatusValues(st, expectedStats);
})();

jsTest.log("Failed single shard transaction.");
(() => {
    startSingleShardTransaction();

    abortFromUnderneath(st, session);
    assert.commandFailedWithCode(session.commitTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);

    expectedStats.currentOpen -= 1;
    expectedStats.currentInactive -= 1;
    expectedStats.totalAborted += 1;
    expectedStats.abortCause["NoSuchTransaction"] = 1;
    expectedStats.commitTypes.singleShard.initiated += 1;
    expectedStats.totalParticipantsAtCommit += 1;
    // The one shard is targeted for the commit then the implicit abort.
    expectedStats.totalRequestsTargeted += 1 + 1;
    verifyServerStatusValues(st, expectedStats);
})();

// TODO (SERVER-48340): Re-enable the single-write-shard transaction commit optimization.
jsTest.log("Successful single write shard transaction.");
(() => {
    startSingleWriteShardTransaction();

    assert.commandWorked(session.commitTransaction_forTesting());

    expectedStats.currentOpen -= 1;
    expectedStats.currentInactive -= 1;
    expectedStats.totalCommitted += 1;
    expectedStats.commitTypes.twoPhaseCommit.initiated += 1;
    expectedStats.commitTypes.twoPhaseCommit.successful += 1;
    expectedStats.totalParticipantsAtCommit += 2;
    expectedStats.totalRequestsTargeted += 1;
    verifyServerStatusValues(st, expectedStats);
})();

// TODO (SERVER-48340): Re-enable the single-write-shard transaction commit optimization.
jsTest.log("Failed single write shard transaction.");
(() => {
    startSingleWriteShardTransaction();

    abortFromUnderneath(st, session);
    assert.commandFailedWithCode(session.commitTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);

    expectedStats.currentOpen -= 1;
    expectedStats.currentInactive -= 1;
    expectedStats.totalAborted += 1;
    expectedStats.abortCause["NoSuchTransaction"] += 1;
    expectedStats.commitTypes.twoPhaseCommit.initiated += 1;
    expectedStats.totalParticipantsAtCommit += 2;
    // There are no implicit aborts after two phase commit, so the coordinator is targeted once.
    expectedStats.totalRequestsTargeted += 1;
    verifyServerStatusValues(st, expectedStats);
})();

jsTest.log("Successful read only transaction.");
(() => {
    startReadOnlyTransaction();

    assert.commandWorked(session.commitTransaction_forTesting());

    expectedStats.currentOpen -= 1;
    expectedStats.currentInactive -= 1;
    expectedStats.totalCommitted += 1;
    expectedStats.commitTypes.readOnly.initiated += 1;
    expectedStats.commitTypes.readOnly.successful += 1;
    expectedStats.totalParticipantsAtCommit += 2;
    expectedStats.totalRequestsTargeted += 2;
    verifyServerStatusValues(st, expectedStats);
})();

jsTest.log("Failed read only transaction.");
(() => {
    startReadOnlyTransaction();

    abortFromUnderneath(st, session);
    assert.commandFailedWithCode(session.commitTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);

    expectedStats.currentOpen -= 1;
    expectedStats.currentInactive -= 1;
    expectedStats.totalAborted += 1;
    expectedStats.abortCause["NoSuchTransaction"] += 1;
    expectedStats.commitTypes.readOnly.initiated += 1;
    expectedStats.totalParticipantsAtCommit += 2;
    // Both shards are targeted for the commit then the implicit abort.
    expectedStats.totalRequestsTargeted += 2 + 2;
    verifyServerStatusValues(st, expectedStats);
})();

jsTest.log("Successful two phase commit transaction.");
(() => {
    startTwoPhaseCommitTransaction();

    assert.commandWorked(session.commitTransaction_forTesting());

    expectedStats.currentOpen -= 1;
    expectedStats.currentInactive -= 1;
    expectedStats.totalCommitted += 1;
    expectedStats.commitTypes.twoPhaseCommit.initiated += 1;
    expectedStats.commitTypes.twoPhaseCommit.successful += 1;
    expectedStats.totalParticipantsAtCommit += 2;
    expectedStats.totalRequestsTargeted += 1;
    verifyServerStatusValues(st, expectedStats);

    // Remove the inserted documents.
    assert.commandWorked(sessionDB[collName].remove({skey: {$in: [-5, 5]}}));
})();

jsTest.log("Failed two phase commit transaction.");
(() => {
    startTwoPhaseCommitTransaction();

    abortFromUnderneath(st, session);
    assert.commandFailedWithCode(session.commitTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);

    expectedStats.currentOpen -= 1;
    expectedStats.currentInactive -= 1;
    expectedStats.totalAborted += 1;
    expectedStats.abortCause["NoSuchTransaction"] += 1;
    expectedStats.commitTypes.twoPhaseCommit.initiated += 1;
    expectedStats.totalParticipantsAtCommit += 2;
    // There are no implicit aborts after two phase commit, so the coordinator is targeted once.
    expectedStats.totalRequestsTargeted += 1;
    verifyServerStatusValues(st, expectedStats);
})();

jsTest.log("Recover successful commit result.");
(() => {
    const recoveryToken = setUpTransactionToRecoverCommit({shouldCommit: true});

    assert.commandWorked(st.s.adminCommand({
        commitTransaction: 1,
        lsid: otherRouterSession.getSessionId(),
        txnNumber: otherRouterSession.getTxnNumber_forTesting(),
        autocommit: false,
        recoveryToken
    }));

    // Note that current open and inactive aren't incremented by setUpTransactionToRecoverCommit.
    expectedStats.totalStarted += 1;
    expectedStats.totalCommitted += 1;
    expectedStats.commitTypes.recoverWithToken.initiated += 1;
    expectedStats.commitTypes.recoverWithToken.successful += 1;
    // The participant stats shouldn't increase if we're recovering commit.
    expectedStats.totalRequestsTargeted += 1;
    verifyServerStatusValues(st, expectedStats);
})();

jsTest.log("Recover failed commit result.");
(() => {
    const recoveryToken = setUpTransactionToRecoverCommit({shouldCommit: false});

    assert.commandFailedWithCode(st.s.adminCommand({
        commitTransaction: 1,
        lsid: otherRouterSession.getSessionId(),
        txnNumber: otherRouterSession.getTxnNumber_forTesting(),
        autocommit: false,
        recoveryToken
    }),
                                 ErrorCodes.NoSuchTransaction);

    // Note that current open and inactive aren't incremented by setUpTransactionToRecoverCommit.
    expectedStats.totalStarted += 1;
    expectedStats.totalAborted += 1;
    expectedStats.abortCause["NoSuchTransaction"] += 1;
    expectedStats.commitTypes.recoverWithToken.initiated += 1;
    // The participant stats shouldn't increase if we're recovering commit.
    // There are no implicit aborts during commit recovery, so the recovery shard is targeted
    // once.
    expectedStats.totalRequestsTargeted += 1;
    verifyServerStatusValues(st, expectedStats);
})();

jsTest.log("Empty recovery token.");
(() => {
    otherRouterSession.startTransaction();
    let resWithEmptyRecoveryToken =
        assert.commandWorked(otherRouterSessionDB.runCommand({find: collName}));
    assert.commandWorked(otherRouterSession.commitTransaction_forTesting());

    // The stats on the main mongos shouldn't have changed.
    verifyServerStatusValues(st, expectedStats);

    assert.commandFailedWithCode(st.s.adminCommand({
        commitTransaction: 1,
        lsid: otherRouterSession.getSessionId(),
        txnNumber: otherRouterSession.getTxnNumber_forTesting(),
        autocommit: false,
        recoveryToken: resWithEmptyRecoveryToken.recoveryToken
    }),
                                 ErrorCodes.NoSuchTransaction);

    expectedStats.currentOpen += 1;
    expectedStats.currentInactive += 1;
    expectedStats.totalStarted += 1;
    expectedStats.commitTypes.recoverWithToken.initiated += 1;
    // No requests are targeted and the decision isn't learned, so total committed/aborted and
    // total requests sent shouldn't change.
    verifyServerStatusValues(st, expectedStats);
})();

jsTest.log("Explicitly aborted transaction.");
(() => {
    session.startTransaction();
    assert.commandWorked(sessionDB[collName].insert({skey: uniqueSkey(), x: 2}));

    expectedStats.currentOpen += 1;
    expectedStats.currentInactive += 1;
    expectedStats.totalStarted += 1;
    expectedStats.totalContactedParticipants += 1;
    expectedStats.totalRequestsTargeted += 1;
    verifyServerStatusValues(st, expectedStats);

    assert.commandWorked(session.abortTransaction_forTesting());

    expectedStats.currentOpen -= 1;
    expectedStats.currentInactive -= 1;
    expectedStats.totalAborted += 1;
    expectedStats.abortCause["abort"] = 1;
    expectedStats.totalRequestsTargeted += 1;
    verifyServerStatusValues(st, expectedStats);
})();

jsTest.log("Implicitly aborted transaction.");
(() => {
    assert.commandWorked(sessionDB[collName].insert({_id: 1, skey: 1}));

    session.startTransaction();
    assert.commandFailedWithCode(sessionDB[collName].insert({_id: 1, skey: 1}),
                                 ErrorCodes.DuplicateKey);

    expectedStats.totalStarted += 1;
    expectedStats.totalAborted += 1;
    expectedStats.abortCause["DuplicateKey"] = 1;
    expectedStats.totalContactedParticipants += 1;
    expectedStats.totalRequestsTargeted += 2;  // Plus one for the implicit abort.
    verifyServerStatusValues(st, expectedStats);

    assert.commandFailedWithCode(session.abortTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);

    // A failed abortTransaction leads to an implicit abort, so two requests are targeted.
    expectedStats.totalRequestsTargeted += 2;
    verifyServerStatusValues(st, expectedStats);
})();

jsTest.log("Abandoned transaction.");
(() => {
    session.startTransaction();
    assert.commandWorked(sessionDB[collName].insert({skey: -15}));

    expectedStats.currentOpen += 1;
    expectedStats.currentInactive += 1;
    expectedStats.totalStarted += 1;
    expectedStats.totalContactedParticipants += 1;
    expectedStats.totalRequestsTargeted += 1;
    verifyServerStatusValues(st, expectedStats);

    session.startTransaction_forTesting({}, {ignoreActiveTxn: true});
    assert.commandWorked(sessionDB[collName].insert({skey: -15}));

    // Note that overwriting a transaction will end the previous transaction from a diagnostics
    // perspective.
    expectedStats.totalStarted += 1;
    expectedStats.totalContactedParticipants += 1;
    expectedStats.totalRequestsTargeted += 1;
    // The router never learned if the previous transaction committed or aborted, so the aborted
    // counter shouldn't be incremented.
    verifyServerStatusValues(st, expectedStats);

    // Abort to clear the shell's session state.
    assert.commandWorked(session.abortTransaction_forTesting());

    expectedStats.currentOpen -= 1;
    expectedStats.currentInactive -= 1;
    expectedStats.totalAborted += 1;
    expectedStats.abortCause["abort"] += 1;
    expectedStats.totalRequestsTargeted += 1;
    verifyServerStatusValues(st, expectedStats);
})();

jsTest.log("Active transaction.");
(() => {
    assert.commandWorked(st.rs0.getPrimary().adminCommand(
        {configureFailPoint: "waitInFindBeforeMakingBatch", mode: "alwaysOn", data: {nss: ns}}));

    const txnThread = new Thread(function(host, dbName, collName) {
        const mongosConn = new Mongo(host);
        const threadSession = mongosConn.startSession();

        threadSession.startTransaction();
        assert.commandWorked(
            threadSession.getDatabase(dbName).runCommand({find: collName, filter: {}}));
        assert.commandWorked(threadSession.abortTransaction_forTesting());
        threadSession.endSession();
    }, st.s.host, dbName, collName);
    txnThread.start();

    // Wait until we know the failpoint has been reached.
    waitForCurOpByFailPointNoNS(st.rs0.getPrimary().getDB("admin"), "waitInFindBeforeMakingBatch");

    expectedStats.currentOpen += 1;
    expectedStats.currentActive += 1;
    expectedStats.totalStarted += 1;
    expectedStats.totalContactedParticipants += 2;
    expectedStats.totalRequestsTargeted += 2;
    verifyServerStatusValues(st, expectedStats);

    assert.commandWorked(st.rs0.getPrimary().adminCommand(
        {configureFailPoint: "waitInFindBeforeMakingBatch", mode: "off"}));
    txnThread.join();

    expectedStats.currentOpen -= 1;
    expectedStats.currentActive -= 1;
    expectedStats.totalAborted += 1;
    expectedStats.abortCause["abort"] += 1;
    expectedStats.totalRequestsTargeted += 2;
    verifyServerStatusValues(st, expectedStats);
})();

const retrySession = st.s.startSession({retryWrites: true});
const retrySessionDB = retrySession.getDatabase(dbName);

jsTest.log("Change shard key with retryable write - findAndModify.");
(() => {
    // Insert document to be updated.
    assert.commandWorked(retrySessionDB[collName].insert({skey: -10}));

    // Retryable write findAndModify that would change the shard key. Uses a txn internally. Throws
    // on error.
    retrySessionDB[collName].findAndModify({query: {skey: -10}, update: {$set: {skey: 10}}});

    expectedStats.totalStarted += 1;
    expectedStats.totalCommitted += 1;
    expectedStats.commitTypes.twoPhaseCommit.initiated += 1;
    expectedStats.commitTypes.twoPhaseCommit.successful += 1;
    expectedStats.totalContactedParticipants += 2;
    expectedStats.totalParticipantsAtCommit += 2;
    expectedStats.totalRequestsTargeted += 4;

    verifyServerStatusValues(st, expectedStats);
})();

jsTest.log("Change shard key with retryable write - batch write command.");
(() => {
    // Insert document to be updated.
    assert.commandWorked(retrySessionDB[collName].insert({skey: -15}));

    // Retryable write update that would change the shard key. Uses a txn internally.
    assert.commandWorked(retrySessionDB[collName].update({skey: -15}, {$set: {skey: 15}}));

    expectedStats.totalStarted += 1;
    expectedStats.totalCommitted += 1;
    expectedStats.commitTypes.twoPhaseCommit.initiated += 1;
    expectedStats.commitTypes.twoPhaseCommit.successful += 1;
    expectedStats.totalContactedParticipants += 2;
    expectedStats.totalParticipantsAtCommit += 2;
    expectedStats.totalRequestsTargeted += 4;

    verifyServerStatusValues(st, expectedStats);
})();

session.endSession();
st.stop();
}());
