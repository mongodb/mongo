/*
 * Utilities for performing a set of retryable or non-retryable internal transactions during or
 * before a resharding operation, and verifying that the transaction history is correctly
 * transferred or not transferred to the recipient shard.
 *
 * Example usage:
 *      const transactionTest = new InternalTransactionReshardingTest();
 *      transactionTest.runTest*(...);
 *      transactionTest.stop();
 *
 * Note that this fixture does not support resetting the collection after each runTest() call so it
 * is illegal to do more than one runTest() call.
 */
'use strict';

load("jstests/libs/discover_topology.js");
load("jstests/sharding/libs/resharding_test_fixture.js");
load('jstests/sharding/libs/sharded_transactions_helpers.js');

function InternalTransactionReshardingTest({reshardInPlace}) {
    jsTest.log(`Running resharding test with options ${tojson({reshardInPlace})}`);

    const reshardingTest =
        new ReshardingTest({numDonors: 2, numRecipients: 1, reshardInPlace, oplogSize: 256});
    reshardingTest.setup();

    const donorShardNames = reshardingTest.donorShardNames;
    const recipientShardNames = reshardingTest.recipientShardNames;
    const reshardingOptions = {
        primaryShardName: donorShardNames[0],
        oldShardKeyPattern: {oldShardKey: 1},
        oldShardKeyChunks: [
            {min: {oldShardKey: MinKey}, max: {oldShardKey: 0}, shard: donorShardNames[0]},
            {min: {oldShardKey: 0}, max: {oldShardKey: MaxKey}, shard: donorShardNames[1]}
        ],
        newShardKeyPattern: {newShardKey: 1},
        newShardKeyChunks: [
            {min: {newShardKey: MinKey}, max: {newShardKey: MaxKey}, shard: recipientShardNames[0]}
        ],
    };

    const kSize10MB = 10 * 1024 * 1024;
    const kInternalTxnType = {kRetryable: 1, kNonRetryable: 2};

    const kDbName = "testDb";
    const kCollName = "testColl";
    const kNs = kDbName + "." + kCollName;

    const mongosTestColl = reshardingTest.createShardedCollection({
        ns: kNs,
        shardKeyPattern: reshardingOptions.oldShardKeyPattern,
        chunks: reshardingOptions.oldShardKeyChunks,
        primaryShardName: reshardingOptions.primaryShardName,
    });
    const mongosConn = mongosTestColl.getMongo();

    let lastTestId = 0;

    function makeSessionOptionsForTest(testId) {
        const sessionUUID = UUID();
        const parentLsid = {id: sessionUUID};
        const parentTxnNumber = testId;
        const childLsidForRetryableWrite = {
            id: sessionUUID,
            txnNumber: NumberLong(parentTxnNumber),
            txnUUID: UUID()
        };
        const childTxnNumberForRetryableWrite = 0;
        const childLsidForNonRetryableWrite = {id: sessionUUID, txnUUID: UUID()};
        const childTxnNumberForNonRetryableWrite = 0;

        return {
            sessionUUID,
            parentLsid,
            parentTxnNumber,
            childLsidForRetryableWrite,
            childTxnNumberForRetryableWrite,
            childLsidForNonRetryableWrite,
            childTxnNumberForNonRetryableWrite
        };
    }

    // Helpers for defining transaction test cases.

    function makeTransactionOptionsForInsertUpdateDeleteTest(
        {isPreparedTxn, isLargeTxn, abortOnInitialTry}) {
        const testId = ++lastTestId;

        const testCase = makeSessionOptionsForTest(testId);
        testCase.id = testId;
        testCase.isPreparedTxn = isPreparedTxn;
        testCase.isLargeTxn = isLargeTxn;
        testCase.abortOnInitialTry = abortOnInitialTry;
        testCase.lastUsedStmtId = -1;

        testCase.commands = [];

        // Define the retryable insert, update and delete statements to be run in the test internal
        // transaction. Prior to resharding, these write statements will be routed to donor0.
        const docToInsert = {insertOp: -testId, oldShardKey: -testId, newShardKey: -testId};
        testCase.commands.push({
            cmdObj: {
                insert: kCollName,
                documents: [docToInsert],
                stmtId: NumberInt(++testCase.lastUsedStmtId)
            },
            checkResponseFunc: (res) => {
                assert.eq(res.n, 1);
            }
        });

        const docToUpdate = {updateOp: -testId, oldShardKey: -testId, newShardKey: -testId};
        testCase.commands.push({
            cmdObj: {
                update: kCollName,
                updates: [{q: docToUpdate, u: {$mul: {updateOp: 100}}}],
                stmtId: NumberInt(++testCase.lastUsedStmtId)
            },
            checkResponseFunc: (res) => {
                assert.eq(res.n, 1);
                assert.eq(res.nModified, 1);
            }
        });
        const updatedDoc = {updateOp: -testId * 100, oldShardKey: -testId, newShardKey: -testId};

        const docToDelete = {deleteOp: -testId, oldShardKey: -testId, newShardKey: -testId};
        testCase.commands.push({
            cmdObj: {
                delete: kCollName,
                deletes: [{q: docToDelete, limit: 1}],
                stmtId: NumberInt(++testCase.lastUsedStmtId)
            },
            checkResponseFunc: (res) => {
                assert.eq(res.n, 1);
            }
        });

        // If testing a prepared and/or large transaction, define additional insert statements to
        // make the transaction a prepared and/or large transaction.
        const additionalDocsToInsert =
            makeInsertCommandsIfTestingPreparedOrLargeTransaction(testId, testCase);

        testCase.setUpFunc = () => {
            const coll = mongosConn.getCollection(kNs);
            assert.commandWorked(coll.insert(docToUpdate));
            assert.commandWorked(coll.insert(docToDelete));
        };

        testCase.checkDocsFunc = (isTxnCommitted) => {
            const coll = mongosConn.getCollection(kNs);
            const docs = coll.find().toArray();

            assert.eq(coll.find(docToInsert).itcount(), isTxnCommitted ? 1 : 0, tojson(docs));
            assert.eq(coll.find(docToUpdate).itcount(), isTxnCommitted ? 0 : 1, tojson(docs));
            assert.eq(coll.find(updatedDoc).itcount(), isTxnCommitted ? 1 : 0, tojson(docs));
            assert.eq(coll.find(docToDelete).itcount(), isTxnCommitted ? 0 : 1, tojson(docs));

            additionalDocsToInsert.forEach(docToInsert => {
                assert.eq(coll.find(docToInsert).itcount(), isTxnCommitted ? 1 : 0, tojson(docs));
            });
        };

        return testCase;
    }

    function makeInsertCommandsIfTestingPreparedOrLargeTransaction(testId, testCase) {
        const additionalDocsToInsert = [];
        if (testCase.isLargeTxn) {
            // Prior to resharding, the insert statements below will be routed to donor0.
            const numLargeDocs = 2;
            for (let i = 0; i < numLargeDocs; i++) {
                const docToInsert = {
                    insert10MB: i.toString() + new Array(kSize10MB).join("x"),
                    oldShardKey: -testId,
                    newShardKey: -testId
                };
                testCase.commands.push({
                    // Use stmtId -1 to get test coverage for "applyOps" entries without a stmtId.
                    cmdObj: {insert: kCollName, documents: [docToInsert], stmtId: NumberInt(-1)},
                    checkResponseFunc: (res) => {
                        assert.eq(res.n, 1);
                    }
                });
                additionalDocsToInsert.push(docToInsert);
            }
        }
        if (testCase.isPreparedTxn) {
            // Prior to resharding, this insert statement below will be routed to donor1.
            const docToInsert = {insertOp: testId, oldShardKey: testId, newShardKey: testId};
            testCase.commands.push({
                cmdObj: {
                    insert: kCollName,
                    documents: [docToInsert],
                    stmtId: NumberInt(++testCase.lastUsedStmtId)
                },
                checkResponseFunc: (res) => {
                    assert.eq(res.n, 1);
                }
            });
            additionalDocsToInsert.push(docToInsert);
        }
        return additionalDocsToInsert;
    }

    function commitTransaction(lsid, txnNumber) {
        assert.commandWorked(mongosConn.adminCommand(makeCommitTransactionCmdObj(lsid, txnNumber)));
    }

    function abortTransaction(lsid, txnNumber, isPreparedTxn) {
        if (!isPreparedTxn) {
            assert.commandWorked(
                mongosConn.adminCommand(makeAbortTransactionCmdObj(lsid, txnNumber)));
        } else {
            const topology = DiscoverTopology.findConnectedNodes(mongosConn);
            const donor1Conn =
                new Mongo(topology.shards[reshardingTest.donorShardNames[1]].primary);
            let fp = configureFailPoint(donor1Conn, "failCommand", {
                failInternalCommands: true,
                failCommands: ["prepareTransaction"],
                errorCode: ErrorCodes.NoSuchTransaction,
            });
            assert.commandFailedWithCode(
                mongosConn.adminCommand(makeCommitTransactionCmdObj(lsid, txnNumber)),
                ErrorCodes.NoSuchTransaction);
            fp.off();
        }
    }

    function getTransactionSessionId(txnType, testCase) {
        return Object.assign({},
                             txnType == kInternalTxnType.kRetryable
                                 ? testCase.childLsidForRetryableWrite
                                 : testCase.childLsidForNonRetryableWrite);
    }

    function getNextTxnNumber(txnType, testCase) {
        return NumberLong(txnType == kInternalTxnType.kRetryable
                              ? ++testCase.childTxnNumberForRetryableWrite
                              : ++testCase.childTxnNumberForNonRetryableWrite);
    }

    /*
     * Runs the commands defined inside 'testCase' in an internal transaction of the specified
     * type.
     */
    function runInternalTransactionOnInitialTry(txnType, testCase) {
        jsTest.log(`Running write statements in an internal transaction with options ${tojson({
            id: testCase.id,
            isPreparedTxn: testCase.isPreparedTxn,
            isLargeTxn: testCase.isLargeTxn,
            abortOnInitialTry: testCase.abortOnInitialTry,
            imageType: testCase.imageType
        })}`);
        testCase.setUpFunc();

        const lsid = getTransactionSessionId(txnType, testCase);

        while (true) {
            const txnNumber = getNextTxnNumber(txnType, testCase);

            try {
                for (let i = 0; i < testCase.commands.length; i++) {
                    const command = testCase.commands[i];
                    const cmdObj =
                        Object.assign({}, command.cmdObj, {lsid, txnNumber, autocommit: false});
                    if (i == 0) {
                        cmdObj.startTransaction = true;
                    }
                    const res = assert.commandWorked(mongosConn.getDB(kDbName).runCommand(cmdObj));
                    command.checkResponseFunc(res);
                }

                if (testCase.abortOnInitialTry) {
                    abortTransaction(lsid, txnNumber, testCase.isPreparedTxn);
                } else {
                    commitTransaction(lsid, txnNumber);
                }
                break;
            } catch (e) {
                if (e.hasOwnProperty('errorLabels') &&
                    e.errorLabels.includes('TransientTransactionError') &&
                    e.code != ErrorCodes.NoSuchTransaction) {
                    jsTest.log("Failed to run transaction due to a transient error " + tojson(e));
                } else {
                    throw e;
                }
            }
        }

        testCase.checkDocsFunc(!testCase.abortOnInitialTry /* isTxnCommitted */);
    }

    /*
     * Retries the commands defined inside 'testCase' in an internal transaction of the specified
     * type. If this retry is expected to fail, asserts that the command to start the transaction
     * fails with an IncompleteTransactionHistory error.
     */
    function runInternalTransactionOnRetry(
        txnType, testCase, isRetryAfterAbort, expectRetryToSucceed) {
        jsTest.log(
            `Retrying write statements executed in an internal transaction with options ${tojson({
                id: testCase.id,
                isPreparedTxn: testCase.isPreparedTxn,
                isLargeTxn: testCase.isLargeTxn,
                abortOnInitialTry: testCase.abortOnInitialTry,
                imageType: testCase.imageType,
                isRetryAfterAbort: isRetryAfterAbort,
                expectRetryToSucceed: expectRetryToSucceed
            })} in another internal transaction`);

        const lsid = getTransactionSessionId(txnType, testCase);
        // Give the session a different txnUUID to simulate a retry from a different mongos.
        lsid.txnUUID = UUID();

        while (true) {
            const txnNumber = getNextTxnNumber(txnType, testCase);

            try {
                for (let i = 0; i < testCase.commands.length; i++) {
                    const command = testCase.commands[i];

                    if (!isRetryAfterAbort && command.cmdObj.stmtId == -1) {
                        // The transaction has already committed and the statement in this command
                        // is not retryable so do not retry it.
                        continue;
                    }

                    const cmdObj =
                        Object.assign({}, command.cmdObj, {lsid, txnNumber, autocommit: false});
                    if (i == 0) {
                        cmdObj.startTransaction = true;
                    }
                    const res = mongosConn.getDB(kDbName).runCommand(cmdObj);

                    if (expectRetryToSucceed) {
                        assert.commandWorked(res);
                        command.checkResponseFunc(res);
                    } else {
                        assert.commandFailedWithCode(res, ErrorCodes.IncompleteTransactionHistory);
                        return;
                    }
                }

                commitTransaction(lsid, txnNumber);
                break;
            } catch (e) {
                if (e.hasOwnProperty('errorLabels') &&
                    e.errorLabels.includes('TransientTransactionError') &&
                    e.code != ErrorCodes.NoSuchTransaction) {
                    jsTest.log("Failed to run transaction due to a transient error " + tojson(e));
                } else {
                    throw e;
                }
            }
        }

        testCase.checkDocsFunc(true /* isTxnCommitted */);
    }

    /*
     * Retries the commands specified inside 'testCase' as retryable writes. If this retry is
     * expected to fail, asserts that all commands fail with an IncompleteTransactionHistory error.
     */
    function runRetryableWriteOnRetry(testCase, expectRetryToSucceed) {
        jsTest.log(
            `Retrying write statements executed in an internal transaction with options ${tojson({
                id: testCase.id,
                isPreparedTxn: testCase.isPreparedTxn,
                isLargeTxn: testCase.isLargeTxn,
                abortOnInitialTry: testCase.abortOnInitialTry,
                imageType: testCase.imageType,
                expectRetryToSucceed: expectRetryToSucceed
            })} in a retryable write`);

        const lsid = testCase.parentLsid;
        const txnNumber = NumberLong(testCase.parentTxnNumber);

        for (let i = 0; i < testCase.commands.length; i++) {
            const command = testCase.commands[i];

            if (command.cmdObj.stmtId == -1) {
                // The statement in this command is not retryable so do not retry it.
                continue;
            }

            const cmdObj = Object.assign({}, command.cmdObj, {lsid, txnNumber});
            const res = mongosConn.getDB(kDbName).runCommand(cmdObj);

            if (expectRetryToSucceed) {
                assert.commandWorked(res);
                command.checkResponseFunc(res);
            } else {
                assert.commandFailedWithCode(res, ErrorCodes.IncompleteTransactionHistory);
            }
        }

        testCase.checkDocsFunc(!testCase.abortOnInitialTry ||
                               expectRetryToSucceed /* isTxnCommitted */);
    }

    /*
     * Runs the commands defined in each 'testCase' inside an internal transaction of the specified
     * type while resharding is running in the background.
     * - If the type is non-retryable, verifies that none of the transactions have a
     *   config.transactions entry on the recipient if the recipient is not also a donor.
     * - If the type is retryable, verifies that the retryable write statements in each transaction
     *   are retryable on the recipient after resharding completes regardless of whether the
     *   transaction committed or aborted on the donor(s).
     */
    function testTransactionsDuringResharding(txnType, testCases) {
        reshardingTest.withReshardingInBackground(
            {
                newShardKeyPattern: reshardingOptions.newShardKeyPattern,
                newChunks: reshardingOptions.newShardKeyChunks,
            },
            () => {
                // The cloneTimestamp is the boundary for whether a retryable write statement will
                // be retryable after the resharding operation completes.
                assert.soon(() => {
                    const coordinatorDoc =
                        mongosConn.getCollection("config.reshardingOperations").findOne({ns: kNs});

                    return coordinatorDoc !== null && coordinatorDoc.cloneTimestamp !== undefined;
                });

                jsTest.log("Start running retryable internal transactions during resharding");
                for (let testCase of testCases) {
                    runInternalTransactionOnInitialTry(txnType, testCase);
                }
                jsTest.log("Finished running retryable internal transactions during resharding");
            });

        const recipientRst = reshardingTest.getReplSetForShard(recipientShardNames[0]);

        if (txnType == kInternalTxnType.kNonRetryable) {
            if (!reshardInPlace) {
                for (let testCase of testCases) {
                    const configTxnEntries = getTxnEntriesForSession(
                        recipientRst, testCase.childLsidForNonRetryableWrite);
                    assert.eq(configTxnEntries.length, 0, tojson(configTxnEntries));
                }
            }
            return;
        }

        // Refresh the mongos so that write statements are routed to the recipient on retries.
        assert.commandWorked(mongosConn.adminCommand({flushRouterConfig: 1}));
        const expectRetryToSucceed = true;

        jsTest.log("Start retrying retryable internal transactions after resharding");
        for (let testCase of testCases) {
            runInternalTransactionOnRetry(txnType,
                                          testCase,
                                          testCase.abortOnInitialTry /* isRetryAfterAbort */,
                                          expectRetryToSucceed);
            // Also retry the write statements as retryable writes.
            runRetryableWriteOnRetry(testCase, expectRetryToSucceed);
        }
        jsTest.log("Finished retrying retryable internal transactions after resharding");

        recipientRst.stopSet(null /* signal */, true /*forRestart */);
        recipientRst.startSet({restart: true});
        recipientRst.getPrimary();

        jsTest.log("Start retrying retryable internal transactions after restarting the recipient");
        for (let testCase of testCases) {
            runInternalTransactionOnRetry(
                txnType, testCase, false /* isRetryAfterAbort */, expectRetryToSucceed);
            // Also retry the write statements as retryable writes.
            runRetryableWriteOnRetry(testCase, expectRetryToSucceed);
        }
        jsTest.log("Finished retrying retryable internal transactions after restart the recipient");
    }

    /*
     * Runs the commands specified in each 'testCase' inside an internal transaction of the
     * specified and then runs resharding to completion.
     * - If the type is non-retryable, verifies that none of the transactions have a
     *   config.transactions entry on the recipient if the recipient is not also a donor.
     * - If the type is retryable, verifies that none of the retryable write statements in each
     *   transaction are retryable on the recipient unless the transaction aborts without prepare
     *   on the donor.
     */
    function testTransactionsBeforeResharding(txnType, testCases) {
        jsTest.log("Start running retryable internal transactions before resharding");
        for (let testCase of testCases) {
            runInternalTransactionOnInitialTry(txnType, testCase);
        }
        jsTest.log("Finished running retryable internal transactions before resharding");

        reshardingTest.withReshardingInBackground({
            newShardKeyPattern: reshardingOptions.newShardKeyPattern,
            newChunks: reshardingOptions.newShardKeyChunks,
        });

        const recipientRst = reshardingTest.getReplSetForShard(recipientShardNames[0]);

        if (txnType == kInternalTxnType.kNonRetryable) {
            if (!reshardInPlace) {
                for (let testCase of testCases) {
                    const configTxnEntries = getTxnEntriesForSession(
                        recipientRst, testCase.childLsidForNonRetryableWrite);
                    assert.eq(configTxnEntries.length, 0, tojson(configTxnEntries));
                }
            }
            return;
        }

        // Refresh the mongos so that write statements are routed to the recipient on retries.
        assert.commandWorked(mongosConn.adminCommand({flushRouterConfig: 1}));
        // To make retryable writes and transactions that existed prior to resharding not retryable
        // after resharding, each recipient performs a snapshot read of the the config.transactions
        // collection and then writes a dead-end noop oplog entry for every retryable write and
        // transaction that it finds. Therefore, a transaction that does not have a
        // config.transactions entry (i.e. an aborted unprepared transaction) is expected to be
        // retryable after resharding.
        let expectRetryForTestCaseToSucceed = (testCase) =>
            !testCase.isPreparedTxn && testCase.abortOnInitialTry;

        jsTest.log("Start retrying retryable internal transactions after resharding");
        for (let testCase of testCases) {
            const expectRetryToSucceed = expectRetryForTestCaseToSucceed(testCase);
            runInternalTransactionOnRetry(txnType,
                                          testCase,
                                          testCase.abortOnInitialTry /* isRetryAfterAbort */,
                                          expectRetryToSucceed);
            // Also retry the write statements as retryable writes.
            runRetryableWriteOnRetry(testCase, expectRetryToSucceed);
        }
        jsTest.log("Finished retrying retryable internal transactions after resharding");

        recipientRst.stopSet(null /* signal */, true /*forRestart */);
        recipientRst.startSet({restart: true});
        recipientRst.getPrimary();

        jsTest.log("Start retrying retryable internal transactions after restarting the recipient");
        for (let testCase of testCases) {
            const expectRetryToSucceed = expectRetryForTestCaseToSucceed(testCase);
            runInternalTransactionOnRetry(
                txnType, testCase, false /* isRetryAfterAbort */, expectRetryToSucceed);
            // Also retry the write statements as retryable writes.
            runRetryableWriteOnRetry(testCase, expectRetryToSucceed);
        }
        jsTest.log(
            "Finished retrying retryable internal transactions after restarting the recipient");
    }

    this.InternalTxnType = kInternalTxnType;

    // Helpers for testing retryable internal transactions with resharding. Each helper defines
    // transaction test cases using the makeTransactionOptions*() helpers above and runs one
    // of the testTransactions*Resharding() helpers above.

    this.runTestForInsertUpdateDeleteDuringResharding = function(txnType, abortOnInitialTry) {
        const testCases = [];
        for (let isPreparedTxn of [true, false]) {
            for (let isLargeTxn of [true, false]) {
                testCases.push(makeTransactionOptionsForInsertUpdateDeleteTest(
                    {isPreparedTxn, isLargeTxn, abortOnInitialTry}));
            }
        }
        testTransactionsDuringResharding(txnType, testCases);
    };

    this.runTestForInsertUpdateDeleteBeforeResharding = function(txnType, abortOnInitialTry) {
        const testCases = [];
        for (let isPreparedTxn of [true, false]) {
            for (let isLargeTxn of [true, false]) {
                testCases.push(makeTransactionOptionsForInsertUpdateDeleteTest(
                    {isPreparedTxn, isLargeTxn, abortOnInitialTry}));
            }
        }
        testTransactionsBeforeResharding(txnType, testCases);
    };

    this.stop = function() {
        reshardingTest.teardown();
    };
}
