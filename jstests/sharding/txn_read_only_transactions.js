/**
 * Tests that the appropriate commit path (single-shard, read-only, multi-shard) is taken for a
 * variety of transaction types.
 *
 * Checks that the response formats are correct across each type for several scenarios, including
 * no failures, a participant having failed over, a participant being unable to satisfy the client's
 * writeConcern, and an invalid client writeConcern.
 *
 * @tags: [uses_transactions, uses_multi_shard_transaction]
 */

(function() {
    'use strict';

    load("jstests/libs/write_concern_util.js");
    load("jstests/sharding/libs/sharded_transactions_helpers.js");

    // Waits for the given log to appear a number of times in the shell's rawMongoProgramOutput.
    // Loops because it is not guaranteed the program output will immediately contain all lines
    // logged at an earlier wall clock time.
    function waitForLog(logLine, times) {
        assert.soon(function() {
            const matches = rawMongoProgramOutput().match(new RegExp(logLine, "g")) || [];
            return matches.length === times;
        }, 'Failed to find "' + logLine + '" logged ' + times + ' times');
    }

    const addTxnFields = function(command, lsid, txnNumber) {
        const txnFields = {
            lsid: lsid,
            txnNumber: NumberLong(txnNumber),
            stmtId: NumberInt(0),
            autocommit: false,
        };
        return Object.assign({}, command, txnFields);
    };

    const defaultCommitCommand = {
        commitTransaction: 1,
        writeConcern: {w: "majority", wtimeout: 3000}
    };
    const noop = () => {};

    const dbName = "test";
    const collName = "foo";
    const ns = dbName + "." + collName;

    let lsid = {id: UUID()};
    let txnNumber = 0;

    let st = new ShardingTest({
        rs0: {nodes: [{}, {rsConfig: {priority: 0}}]},
        rs1: {nodes: [{}, {rsConfig: {priority: 0}}]},
        config: 1,
        other: {mongosOptions: {verbose: 3}},
    });

    assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
    assert.commandWorked(st.s.adminCommand({movePrimary: dbName, to: st.shard1.shardName}));

    // Create a "dummy" collection for doing noop writes to advance shard's last applied OpTimes.
    assert.commandWorked(st.s.getDB(dbName).getCollection("dummy").insert({dummy: 1}));

    // Create a sharded collection with a chunk on each shard:
    // shard0: [-inf, 0)
    // shard1: [0, 10)
    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));
    assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 0}}));
    assert.commandWorked(
        st.s.adminCommand({moveChunk: ns, find: {_id: -1}, to: st.shard0.shardName}));

    flushRoutersAndRefreshShardMetadata(st, {ns});

    // For each transaction type, contains the list of statements for that type.
    const transactionTypes = {
        readOnlySingleShardSingleStatement: txnNumber => {
            return [{
                find: collName,
                filter: {_id: txnNumber},
                startTransaction: true,
            }];
        },
        readOnlySingleShardMultiStatement: txnNumber => {
            return [
                {
                  find: collName,
                  filter: {_id: txnNumber},
                  startTransaction: true,
                },
                {distinct: collName, key: "_id", query: {_id: txnNumber}},
            ];
        },
        readOnlyMultiShardSingleStatement: txnNumber => {
            return [{find: collName, startTransaction: true}];
        },
        readOnlyMultiShardMultiStatement: txnNumber => {
            return [
                {
                  find: collName,
                  filter: {_id: txnNumber},
                  startTransaction: true,
                },
                {distinct: collName, key: "_id", query: {_id: (txnNumber * -1)}},
            ];
        },
        writeOnlySingleShardSingleStatement: txnNumber => {
            return [{
                insert: collName,
                documents: [{_id: txnNumber}],
                startTransaction: true,
            }];
        },
        writeOnlySingleShardMultiStatement: txnNumber => {
            return [
                {
                  insert: collName,
                  documents: [{_id: txnNumber}],
                  startTransaction: true,
                },
                {
                  update: collName,
                  updates: [{q: {_id: txnNumber}, u: {$set: {updated: 1}}}],
                }
            ];
        },
        writeOnlyMultiShardSingleStatement: txnNumber => {
            return [{
                insert: collName,
                documents: [{_id: (txnNumber * -1)}, {_id: txnNumber}],
                startTransaction: true,
            }];
        },
        writeOnlyMultiShardMultiStatement: txnNumber => {
            return [
                {
                  insert: collName,
                  documents: [{_id: txnNumber}],
                  startTransaction: true,
                },
                {
                  insert: collName,
                  documents: [{_id: (txnNumber * -1)}],
                }
            ];
        },
        readWriteSingleShard: txnNumber => {
            return [
                {
                  find: collName,
                  filter: {_id: txnNumber},
                  startTransaction: true,
                },
                {
                  insert: collName,
                  documents: [{_id: txnNumber}],
                }
            ];
        },
        readWriteMultiShard: txnNumber => {
            return [
                {
                  find: collName,
                  filter: {_id: txnNumber},
                  startTransaction: true,
                },
                {
                  insert: collName,
                  documents: [{_id: (txnNumber * -1)}],
                }
            ];
        },
    };

    const failureModes = {
        noFailures: {
            beforeStatements: noop,
            beforeCommit: noop,
            getCommitCommand: (lsid, txnNumber) => {
                return addTxnFields(defaultCommitCommand, lsid, txnNumber);
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
                assert.commandWorked(
                    st.shard1.adminCommand({replSetStepDown: 1 /* stepDownSecs */, force: true}));
            },
            getCommitCommand: (lsid, txnNumber) => {
                return addTxnFields(defaultCommitCommand, lsid, txnNumber);
            },
            checkCommitResult: (res) => {
                // Commit should return NoSuchTransaction.
                assert.commandFailedWithCode(res, ErrorCodes.NoSuchTransaction);
                assert.eq(["TransientTransactionError"], res.errorLabels);
            },
            cleanUp: () => {
                st.rs1.awaitNodesAgreeOnPrimary();
            },
        },
        participantCannotMajorityCommitWritesClientSendsWriteConcernMajority: {
            beforeStatements: () => {
                // Participant cannot majority commit writes.
                stopServerReplication(st.rs1.getSecondaries());

                // Do a write on rs1 through the router outside the transaction to ensure the
                // transaction will choose a read time that has not been majority committed.
                assert.commandWorked(st.s.getDB(dbName).getCollection("dummy").insert({dummy: 1}));
            },
            beforeCommit: noop,
            getCommitCommand: (lsid, txnNumber) => {
                return addTxnFields(defaultCommitCommand, lsid, txnNumber);
            },
            checkCommitResult: (res) => {
                // Commit should return ok with a writeConcernError with wtimeout.
                assert.commandWorkedIgnoringWriteConcernErrors(res);
                checkWriteConcernTimedOut(res);
                assert.eq(null, res.errorLabels);
            },
            cleanUp: () => {
                restartServerReplication(st.rs1.getSecondaries());
            },
        },
        participantCannotMajorityCommitWritesClientSendsWriteConcern1: {
            beforeStatements: () => {
                // Participant cannot majority commit writes.
                stopServerReplication(st.rs1.getSecondaries());

                // Do a write on rs1 through the router outside the transaction to ensure the
                // transaction will choose a read time that has not been majority committed.
                assert.commandWorked(st.s.getDB(dbName).getCollection("dummy").insert({dummy: 1}));
            },
            beforeCommit: noop,
            getCommitCommand: (lsid, txnNumber) => {
                return addTxnFields({commitTransaction: 1, writeConcern: {w: 1}}, lsid, txnNumber);
            },
            checkCommitResult: (res) => {
                // Commit should return ok without writeConcern error
                assert.commandWorked(res);
                assert.eq(null, res.errorLabels);
            },
            cleanUp: () => {
                restartServerReplication(st.rs1.getSecondaries());
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
            checkCommitResult: (res) => {
                // Commit should return ok with writeConcernError without wtimeout.
                assert.commandWorkedIgnoringWriteConcernErrors(res);
                assertWriteConcernError(res);
                assert.eq(ErrorCodes.UnknownReplWriteConcern, res.writeConcernError.code);
                assert.eq(null, res.writeConcernError.errInfo);  // errInfo only set for wtimeout
                assert.eq(null, res.errorLabels);
            },
            cleanUp: noop,
        },
    };

    for (const failureModeName in failureModes) {
        for (const type in transactionTypes) {
            // TODO (SERVER-37881): Unblacklist these test cases once the coordinator times out
            // waiting for votes.
            if (failureModeName.includes("participantCannotMajorityCommitWrites") &&
                (type === "writeOnlyMultiShardSingleStatement" ||
                 type === "writeOnlyMultiShardMultiStatement" || type === "readWriteMultiShard")) {
                jsTest.log(
                    `Testing ${failureModeName} with ${type} is skipped until SERVER-37881 is implemented`);
                continue;
            }

            txnNumber++;
            jsTest.log(`Testing ${failureModeName} with ${type} at txnNumber ${txnNumber}`);

            const failureMode = failureModes[failureModeName];

            // Run the statements.
            failureMode.beforeStatements();
            transactionTypes[type](txnNumber).forEach(command => {
                assert.commandWorked(
                    st.s.getDB(dbName).runCommand(addTxnFields(command, lsid, txnNumber)));
            });

            // Run commit.
            const commitCmd = failureMode.getCommitCommand(lsid, txnNumber);
            failureMode.beforeCommit();
            const commitRes = st.s.adminCommand(commitCmd);
            failureMode.checkCommitResult(commitRes);

            // Re-running commit should return the same response.
            const commitRetryRes = st.s.adminCommand(commitCmd);
            failureMode.checkCommitResult(commitRetryRes);

            if (type.includes("SingleShard")) {
                waitForLog("Committing single-shard transaction", 2);
            } else if (type.includes("readOnly")) {
                waitForLog("Committing read-only transaction", 2);
            } else if (type.includes("MultiShard")) {
                waitForLog("Committing multi-shard transaction", 2);
            } else {
                assert(false, `Unknown transaction type: ${type}`);
            }

            clearRawMongoProgramOutput();

            failureMode.cleanUp();
        }
    }

    st.stop();

})();
