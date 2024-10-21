/**
 * Tests that the appropriate commit path (single-shard, read-only, single-write-shard, two-phase
 * commit) is taken for a variety of transaction types.
 *
 * Checks that the response formats are correct across each type for several scenarios, including
 * no failures, a participant having failed over, a participant being unable to satisfy the client's
 * writeConcern, and an invalid client writeConcern.
 *
 * @tags: [uses_transactions, uses_multi_shard_transaction]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {
    assertWriteConcernError,
    checkWriteConcernTimedOut,
    restartServerReplication,
    stopServerReplication,
} from "jstests/libs/write_concern_util.js";
import {
    enableCoordinateCommitReturnImmediatelyAfterPersistingDecision
} from "jstests/sharding/libs/sharded_transactions_helpers.js";

// Waits for the given log to appear a number of times in the shell's rawMongoProgramOutput.
// Loops because it is not guaranteed the program output will immediately contain all lines
// logged at an earlier wall clock time.
function waitForLog(logLine, times) {
    assert.soon(function() {
        const matches = rawMongoProgramOutput().match(new RegExp(logLine, "g")) || [];
        return matches.length === times;
    }, 'Failed to find "' + logLine + '" logged ' + times + ' times');
}

function getLSID() {
    return {id: UUID()};
}

const addTxnFields = function(command, lsid, txnNumber, startTransaction) {
    let txnFields = {
        lsid: lsid,
        txnNumber: NumberLong(txnNumber),
        stmtId: NumberInt(0),
        autocommit: false,
    };
    if (startTransaction) {
        txnFields.startTransaction = true;
    }
    return Object.assign({}, command, txnFields);
};

const makeCommitCommand = function(wtimeout) {
    let writeConcern = {w: "majority"};
    if (wtimeout) {
        writeConcern.wtimeout = wtimeout;
    }
    return {commitTransaction: 1, writeConcern: writeConcern};
};

const noop = () => {};

const dbName = "test";
const collName = "foo";
const ns = dbName + "." + collName;

const versionSupportsSingleWriteShardCommitOptimization =
    MongoRunner.compareBinVersions(jsTestOptions().mongosBinVersion, "7.1") >= 0;

// Lower the transaction timeout, since this test exercises cases where the coordinator should
// time out collecting prepare votes from participants that cannot majority commit writes.
TestData.transactionLifetimeLimitSeconds = 30;

let st = new ShardingTest({
    shards: 3,
    // Create shards with more than one node because we test for writeConcern majority failing.
    config: TestData.configShard ? undefined : 1,
    rsOptions: {
        setParameter: {
            // Set this to higher than the deault 5ms to avoid failures due to not being able to
            // acquire the lock quickly enough.
            maxTransactionLockRequestTimeoutMillis: ReplSetTest.kDefaultTimeoutMS,
        }
    },
    other: {
        // The name of the shards affects the ordering of which shard will be targeted first
        // for broadcast operations so always use the same names for each test run.
        alwaysUseTestNameForShardName: true,
        mongosOptions: {
            verbose: 3,
            setParameter: {'failpoint.skipClusterParameterRefresh': "{'mode':'alwaysOn'}"}
        },
        rs0: {nodes: [{}, {rsConfig: {priority: 0}}]},
        rs1: {nodes: [{}, {rsConfig: {priority: 0}}]},
        rs2: {nodes: [{}, {rsConfig: {priority: 0}}]},
    },
    // By default, our test infrastructure sets the election timeout to a very high value (24
    // hours). For this test, we need a shorter election timeout because it relies on nodes running
    // an election when they do not detect an active primary. Therefore, we are setting the
    // electionTimeoutMillis to its default value.
    initiateWithDefaultElectionTimeout: true
});

enableCoordinateCommitReturnImmediatelyAfterPersistingDecision(st);
assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard1.shardName}));

// Create a "dummy" collection for doing noop writes to advance shard's last applied OpTimes.
assert.commandWorked(st.s.getDB(dbName).getCollection("dummy").insert({dummy: 1}));

// The test uses three shards with one chunk each in order to control which shards are targeted
// for each statement:
//
// (-inf, 0):                   shard key = txnNumber * -1
// (0, MAX_TRANSACTIONS):       shard key = txnNumber
// (MAX_TRANSACTIONS, +inf):    shard key = txnNumber + MAX_TRANSACTIONS
//
// So, if the test ever exceeds txnNumber transactions, statements that are meant to target the
// middle chunk will instead target the highest chunk. To fix this, increase MAX_TRANSACTIONS.
const MAX_TRANSACTIONS = 10000;

// Create a sharded collection with a chunk on each shard:
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));
assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 0}}));
assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: MAX_TRANSACTIONS}}));
assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {_id: -1}, to: st.shard0.shardName}));
assert.commandWorked(
    st.s.adminCommand({moveChunk: ns, find: {_id: MAX_TRANSACTIONS}, to: st.shard2.shardName}));

// Insert something into each chunk so that a multi-update actually results in a write on each
// shard (otherwise the shard may remain read-only). This also ensures all the routers and
// shards have fresh routing table caches, so they do not need to be refreshed separately.
assert.commandWorked(st.s.getDB(dbName).runCommand({
    insert: collName,
    documents: [{_id: -1 * MAX_TRANSACTIONS}, {_id: 0}, {_id: MAX_TRANSACTIONS}]
}));

let txnNumber = 1;

const readShard0 = txnNumber => {
    return {find: collName, filter: {_id: (-1 * txnNumber)}};
};

const readShard1 = txnNumber => {
    return {find: collName, filter: {_id: txnNumber}};
};

const readShard2 = txnNumber => {
    return {find: collName, filter: {_id: (MAX_TRANSACTIONS + txnNumber)}};
};

const readAllShards = () => {
    return {find: collName};
};

const writeShard0 = txnNumber => {
    return {
        update: collName,
        updates:
            [{q: {_id: (txnNumber * -1)}, u: {_id: (txnNumber * -1), updated: 1}, upsert: true}],
    };
};

const writeShard1 = txnNumber => {
    return {
        update: collName,
        updates: [{q: {_id: txnNumber}, u: {_id: txnNumber, updated: 1}, upsert: true}],
    };
};

const writeShard2 = txnNumber => {
    return {
        update: collName,
        updates: [{
            q: {_id: (txnNumber + MAX_TRANSACTIONS)},
            u: {_id: (txnNumber + MAX_TRANSACTIONS), updated: 1},
            upsert: true
        }],
    };
};

const writeAllShards = () => {
    return {
        update: collName,
        updates: [{q: {}, u: {$inc: {updated: 1}}, multi: true}],
    };
};

// For each transaction type, contains the list of statements for that type.
const transactionTypes = {
    readOnlySingleShardSingleStatementExpectSingleShardCommit: txnNumber => {
        return [readShard0(txnNumber)];
    },
    readOnlySingleShardMultiStatementExpectSingleShardCommit: txnNumber => {
        return [readShard0(txnNumber), readShard0(txnNumber)];
    },
    readOnlyMultiShardSingleStatementExpectReadOnlyCommit: txnNumber => {
        return [readAllShards(txnNumber)];
    },
    readOnlyMultiShardMultiStatementExpectReadOnlyCommit: txnNumber => {
        return [readShard0(txnNumber), readShard1(txnNumber), readShard2(txnNumber)];
    },
    writeSingleShardSingleStatementExpectSingleShardCommit: txnNumber => {
        return [writeShard0(txnNumber)];
    },
    writeSingleShardMultiStatementExpectSingleShardCommit: txnNumber => {
        return [writeShard0(txnNumber), writeShard0(txnNumber)];
    },
    writeMultiShardSingleStatementExpectTwoPhaseCommit: txnNumber => {
        return [writeAllShards(txnNumber)];
    },
    writeMultiShardMultiStatementExpectTwoPhaseCommit: txnNumber => {
        return [writeShard0(txnNumber), writeShard1(txnNumber), writeShard2(txnNumber)];
    },
    readWriteSingleShardExpectSingleShardCommit: txnNumber => {
        return [readShard0(txnNumber), writeShard0(txnNumber)];
    },
    writeReadSingleShardExpectSingleShardCommit: txnNumber => {
        return [writeShard0(txnNumber), readShard0(txnNumber)];
    },
    readOneShardWriteOtherShardExpectSingleWriteShardCommit: txnNumber => {
        return [readShard0(txnNumber), writeShard1(txnNumber)];
    },
    writeOneShardReadOtherShardExpectSingleWriteShardCommit: txnNumber => {
        return [writeShard0(txnNumber), readShard1(txnNumber)];
    },
    readOneShardWriteTwoOtherShardsExpectTwoPhaseCommit: txnNumber => {
        return [readShard0(txnNumber), writeShard1(txnNumber), writeShard2(txnNumber)];
    },
    writeTwoShardsReadOneOtherShardExpectTwoPhaseCommit: txnNumber => {
        return [writeShard0(txnNumber), writeShard1(txnNumber), readShard2(txnNumber)];
    },
};

const failureModes = {
    noFailures: {
        beforeStatements: noop,
        beforeCommit: noop,
        getCommitCommand: (lsid, txnNumber) => {
            return addTxnFields(makeCommitCommand(), lsid, txnNumber);
        },
        checkCommitResult: (res) => {
            // Commit should return ok without writeConcern error
            assert.commandWorked(res);
            assert.eq(null, res.errorLabels);
        },
        cleanUp: noop,
    },
    participantStepsDownBeforeClientSendsCommit: {
        beforeStatements: noop,
        beforeCommit: () => {
            // Participant primary steps down.
            let primary = st.rs0.getPrimary();
            assert.commandWorked(
                primary.adminCommand({replSetStepDown: 60 /* stepDownSecs */, force: true}));
            st.rs0.waitForState(primary, ReplSetTest.State.SECONDARY);
            assert.commandWorked(primary.adminCommand({replSetFreeze: 0}));
            st.rs0.awaitNodesAgreeOnPrimary();
        },
        getCommitCommand: (lsid, txnNumber) => {
            return addTxnFields(makeCommitCommand(), lsid, txnNumber);
        },
        checkCommitResult: (res) => {
            // Commit should return NoSuchTransaction.
            assert.commandFailedWithCode(res, ErrorCodes.NoSuchTransaction);
            assert.eq(["TransientTransactionError"], res.errorLabels);
        },
        cleanUp: () => {
            st.rs0.awaitNodesAgreeOnPrimary();
        },
    },
    participantCannotMajorityCommitWritesClientSendsWriteConcernMajority: {
        beforeStatements: (expectTwoPhaseCommit) => {
            // The default WC is majority and stopServerReplication will prevent satisfying any
            // majority writes.
            assert.commandWorked(st.s.adminCommand({
                setDefaultRWConcern: 1,
                defaultWriteConcern: {w: 1},
                writeConcern: {w: "majority"}
            }));
            // If two-phase commit is involved, rs0 will be the coordinator so we should disable
            // replication on a different participant.
            stopServerReplication(expectTwoPhaseCommit ? st.rs1.getSecondaries()
                                                       : st.rs0.getSecondaries());

            // Do a write on rs0 through the router outside the transaction to ensure the
            // transaction will choose a read time that has not been majority committed.
            assert.commandWorked(st.s.getDB(dbName).getCollection("dummy").insert({dummy: 1}));
        },
        beforeCommit: noop,
        getCommitCommand: (lsid, txnNumber) => {
            return addTxnFields(makeCommitCommand(10 * 1000 /* wtimeout */), lsid, txnNumber);
        },
        checkCommitResult: (res, {
            expectTwoPhaseCommit,
            expectSingleWriteShardCommit,
            singleWriteShardCommitFirstShardReadOnly,
            isRetry,
            lsid,
            txnNumber,
        }) => {
            if (expectTwoPhaseCommit) {
                // One of the participants cannot majority commit writes so the coordinator will
                // timeout waiting for votes, and consequently abort the transaction with
                // NoSuchTransaction error as the abort reason.
                assert.commandFailedWithCode(res, ErrorCodes.NoSuchTransaction);
                assert.eq(["TransientTransactionError"], res.errorLabels);
            } else if (expectSingleWriteShardCommit) {
                if (singleWriteShardCommitFirstShardReadOnly) {
                    if (isRetry) {
                        // Retry of single write shard commit triggers txn recovery, which will
                        // discover the transaction aborted on the first attempt.
                        assert.commandFailedWithCode(res, ErrorCodes.NoSuchTransaction);
                        assert.eq(["TransientTransactionError"], res.errorLabels);
                        return;
                    }

                    // Router returns whatever error the first failed read commit failed with as a
                    // command error, even if it was a write concern error, since nothing durable
                    // could be written. This particular error isn't considered transient since
                    // WriteWriteConcernFailed is not a transient code.
                    assert.commandFailedWithCode(res, ErrorCodes.WriteConcernFailed);
                    assert(!res.writeConcernError, res);
                    assert.eq(null, res.errorLabels);

                    // Any read shard failure should have triggered an implicit abort on all shards.
                    // Note the first shard already received commitTransaction but couldn't majority
                    // commit it, so it should have already committed the transaction.
                    const dummyTxnCmd = addTxnFields({commitTransaction: 1}, lsid, txnNumber);
                    assert.commandWorked(st.rs0.getPrimary().adminCommand(dummyTxnCmd));
                    assert.commandFailedWithCode(st.rs1.getPrimary().adminCommand(dummyTxnCmd),
                                                 ErrorCodes.NoSuchTransaction);
                } else {
                    // In this case, the router receives a write concern error from the write shard,
                    // which means the transaction's effects are written to at least the primary
                    // node, so we can return that shards write concern error with the router's
                    // commit response to the client.
                    assert.commandWorkedIgnoringWriteConcernErrors(res);
                    checkWriteConcernTimedOut(res);
                    assert.eq(null, res.errorLabels);
                }
            } else {
                // Commit should return ok with a writeConcernError with wtimeout.
                assert.commandWorkedIgnoringWriteConcernErrors(res);
                checkWriteConcernTimedOut(res);
                assert.eq(null, res.errorLabels);
            }
        },
        cleanUp: (expectTwoPhaseCommit) => {
            restartServerReplication(expectTwoPhaseCommit ? st.rs1.getSecondaries()
                                                          : st.rs0.getSecondaries());
        },
    },
    participantCannotMajorityCommitWritesClientSendsWriteConcern1: {
        beforeStatements: (expectTwoPhaseCommit) => {
            // stopServerReplication will prevent fulfil any majority writes.
            assert.commandWorked(st.s.adminCommand({
                setDefaultRWConcern: 1,
                defaultWriteConcern: {w: 1},
                writeConcern: {w: "majority"}
            }));
            // If two-phase commit is involved, rs0 will be the coordinator so we should disable
            // replication on a different participant.
            stopServerReplication(expectTwoPhaseCommit ? st.rs1.getSecondaries()
                                                       : st.rs0.getSecondaries());

            // Do a write on rs0 through the router outside the transaction to ensure the
            // transaction will choose a read time that has not been majority committed.
            assert.commandWorked(st.s.getDB(dbName).getCollection("dummy").insert({dummy: 1}));
        },
        beforeCommit: noop,
        getCommitCommand: (lsid, txnNumber) => {
            return addTxnFields({commitTransaction: 1, writeConcern: {w: 1}}, lsid, txnNumber);
        },
        checkCommitResult: (res, {expectTwoPhaseCommit, expectSingleWriteShardCommit}) => {
            if (expectTwoPhaseCommit) {
                // One of the participants cannot majority commit writes so the coordinator will
                // timeout waiting for votes, and consequently abort the transaction with
                // NoSuchTransaction error as the abort reason.
                assert.commandFailedWithCode(res, ErrorCodes.NoSuchTransaction);
                assert.eq(["TransientTransactionError"], res.errorLabels);
            } else if (expectSingleWriteShardCommit) {
                // Both the read only and single write shard phases will use the client's write
                // concern so the commit can succeed without a write concern error.
                assert.commandWorked(res);
                assert.eq(null, res.errorLabels);
            } else {
                // Commit should return ok without writeConcern error.
                assert.commandWorked(res);
                assert.eq(null, res.errorLabels);
            }
        },
        cleanUp: (expectTwoPhaseCommit) => {
            restartServerReplication(expectTwoPhaseCommit ? st.rs1.getSecondaries()
                                                          : st.rs0.getSecondaries());
        },
    },
    clientSendsInvalidWriteConcernOnCommit: {
        beforeStatements: noop,
        beforeCommit: noop,
        getCommitCommand: (lsid, txnNumber) => {
            // Client sends invalid writeConcern on commit.
            return addTxnFields(
                {commitTransaction: 1, writeConcern: {w: "invalid"}}, lsid, txnNumber);
        },
        checkCommitResult: (res, {expectSingleWriteShardCommit, isRetry, lsid, txnNumber}) => {
            if (expectSingleWriteShardCommit) {
                if (isRetry) {
                    // The retry triggers decision recovery which finds the transaction aborted,
                    // and then fails waiting for the invalid write concern.
                    assert.commandFailedWithCode(res, ErrorCodes.NoSuchTransaction);
                    assertWriteConcernError(res);
                    assert.eq(ErrorCodes.UnknownReplWriteConcern, res.writeConcernError.code);
                    assert(!res.writeConcernError.errInfo ||
                           !res.writeConcernError.errInfo.wtimeout);
                    assert.eq(null, res.errorLabels);
                    return;
                }

                // The invalid write concern will fail on a read only shard, which we treat as a
                // command error because the commit cannot be durable on the write shard.
                assert.commandFailedWithCode(res, ErrorCodes.UnknownReplWriteConcern);
                assert(!res.writeConcernError, res);
                assert.eq(null, res.errorLabels, res);
                return;
            }

            // Commit should return ok with writeConcernError without wtimeout.
            assert.commandWorkedIgnoringWriteConcernErrors(res);
            assertWriteConcernError(res);
            assert.eq(ErrorCodes.UnknownReplWriteConcern, res.writeConcernError.code);
            assert(!res.writeConcernError.errInfo || !res.writeConcernError.errInfo.wtimeout);
            assert.eq(null, res.errorLabels);
        },
        cleanUp: noop,
    },
};

for (const failureModeName in failureModes) {
    for (const type in transactionTypes) {
        clearRawMongoProgramOutput();
        const lsid = getLSID();
        txnNumber++;
        assert.lt(txnNumber,
                  MAX_TRANSACTIONS,
                  "Test exceeded maximum number of transactions allowable by the test's chunk" +
                      " distribution created during the test setup. Please increase" +
                      " MAX_TRANSACTIONS in the test.");

        jsTest.log(`Testing ${failureModeName} with ${type} at txnNumber ${txnNumber}`);

        const failureMode = failureModes[failureModeName];

        const commitOpts = {
            expectSingleShardCommit: type.includes("ExpectSingleShardCommit"),
            expectReadOnlyCommit: type.includes("ExpectReadOnlyCommit"),
            expectSingleWriteShardCommit: type.includes("ExpectSingleWriteShardCommit") &&
                versionSupportsSingleWriteShardCommitOptimization,
            expectTwoPhaseCommit: type.includes("ExpectTwoPhaseCommit") ||
                (type.includes("ExpectSingleWriteShardCommit") &&
                 !versionSupportsSingleWriteShardCommitOptimization),
            singleWriteShardCommitFirstShardReadOnly: type.includes("readOneShardWriteOther"),
            lsid,
            txnNumber,
        };

        // Run the statements.
        failureMode.beforeStatements(commitOpts.expectTwoPhaseCommit);
        let startTransaction = true;
        transactionTypes[type](txnNumber).forEach(command => {
            assert.commandWorked(st.s.getDB(dbName).runCommand(
                addTxnFields(command, lsid, txnNumber, startTransaction)));
            startTransaction = false;
        });

        // Run commit.
        const commitCmd = failureMode.getCommitCommand(lsid, txnNumber);
        failureMode.beforeCommit();
        const commitRes = st.s.adminCommand(commitCmd);
        failureMode.checkCommitResult(commitRes, commitOpts);

        // Re-running commit should return the same response.
        const commitRetryRes = st.s.adminCommand(commitCmd);
        commitOpts.isRetry = true;
        failureMode.checkCommitResult(commitRetryRes, commitOpts);

        if (commitOpts.expectSingleShardCommit) {
            waitForLog("Committing single-shard transaction", 2);
        } else if (commitOpts.expectReadOnlyCommit) {
            waitForLog("Committing read-only transaction", 2);
        } else if (commitOpts.expectSingleWriteShardCommit) {
            waitForLog("Committing single-write-shard transaction", 2);
        } else if (commitOpts.expectTwoPhaseCommit) {
            waitForLog("Committing using two-phase commit", 2);
        } else {
            assert(false, `Unknown transaction type: ${type}`);
        }

        clearRawMongoProgramOutput();

        failureMode.cleanUp(commitOpts.expectTwoPhaseCommit);
    }
}

st.stop();
