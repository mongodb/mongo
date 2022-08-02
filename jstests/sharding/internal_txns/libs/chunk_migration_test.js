/*
 * Utilities for performing a set of retryable or non-retryable internal transactions during or
 * before a chunk migration, and verifying that the transaction history is correctly transferred or
 * not transferred to the recipient shard.
 *
 * Example usage:
 *      const transactionTest = new InternalTransactionChunkMigrationTest();
 *      transactionTest.runTest*(...);
 *      transactionTest.stop();
 *
 * Note that this fixture runs each runTest() against its own collection so it is illegal to make
 * more than one runTest() call.
 */
'use strict';

load('jstests/libs/chunk_manipulation_util.js');
load('jstests/sharding/internal_txns/libs/fixture_helpers.js');
load('jstests/sharding/libs/sharded_transactions_helpers.js');

function InternalTransactionChunkMigrationTest(storeFindAndModifyImagesInSideCollection = true) {
    jsTest.log(`Running chunk migration test with options ${
        tojson({storeFindAndModifyImagesInSideCollection})}`);

    let st = new ShardingTest({
        mongos: 1,
        shards: 3,
        rs: {nodes: 2},
        rsOptions: {
            oplogSize: 256,
            setParameter: {
                storeFindAndModifyImagesInSideCollection: storeFindAndModifyImagesInSideCollection,
                maxNumberOfTransactionOperationsInSingleOplogEntry: 1
            }
        }
    });
    let staticMongod = MongoRunner.runMongod({});

    const kInternalTxnType = {kRetryable: 1, kNonRetryable: 2};
    const kImageType = {kPreImage: 1, kPostImage: 2};

    const kDbName = "testDb";
    const kCollNamePrefix = "testColl";
    let collId = 0;

    // Helper for setting up a sharded collection and defining migrations to be run during each
    // test.
    function setUpTestCollection() {
        ++collId;

        const dbName = kDbName;
        const collName = kCollNamePrefix + "-" + collId;
        const ns = dbName + "." + collName;

        // Chunk distribution before migrations:
        //      shard0: [MinKey, 1]
        //      shard1: [0, MaxKey]
        // Chunk distribution after migration0:
        //      shard2: [MinKey, 1]
        //      shard1: [1, MaxKey]
        // Chunk distribution after migration1:
        //      shard1: [MinKey, 1], [1, MaxKey]
        const shardKeyPattern = {shardKey: 1};
        assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: shardKeyPattern}));
        assert.commandWorked(st.s.adminCommand({split: ns, middle: {shardKey: 1}}));
        assert.commandWorked(
            st.s.adminCommand({moveChunk: ns, find: {shardKey: MinKey}, to: st.shard0.shardName}));
        assert.commandWorked(
            st.s.adminCommand({moveChunk: ns, find: {shardKey: 1}, to: st.shard1.shardName}));

        const migration0 = {
            donorShard: st.shard0,
            recipientShard: st.shard2,
            recipientRst: st.rs2,
            cmdObj: {moveChunk: ns, find: {shardKey: 0}, to: st.shard2.shardName}
        };
        const migration1 = {
            donorShard: st.shard2,
            recipientShard: st.shard1,
            recipientRst: st.rs1,
            cmdObj: {moveChunk: ns, find: {shardKey: 0}, to: st.shard1.shardName}
        };

        return {
            dbName,
            collName,
            ns,
            shardKeyPattern,
            migration0,
            migration1,
        };
    }

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
    let lastTestId = 0;

    function makeTransactionOptionsForInsertUpdateDeleteTest(
        dbName, collName, {isPreparedTxn, isLargeTxn, abortOnInitialTry}) {
        const testId = ++lastTestId;

        const testCase = makeSessionOptionsForTest(testId);
        testCase.id = testId;
        testCase.dbName = dbName;
        testCase.collName = collName;
        testCase.isPreparedTxn = isPreparedTxn;
        testCase.isLargeTxn = isLargeTxn;
        testCase.abortOnInitialTry = abortOnInitialTry;
        testCase.lastUsedStmtId = -1;

        testCase.commands = [];

        // Define the retryable insert, update and delete statements to be run in the test internal
        // transaction. Prior to migration0, these write statements will be routed to shard0.
        const docToInsert = {insertOp: -testId, shardKey: -testId};
        testCase.commands.push({
            cmdObj: {
                insert: collName,
                documents: [docToInsert],
                stmtId: NumberInt(++testCase.lastUsedStmtId)
            },
            checkResponseFunc: (res) => {
                assert.eq(res.n, 1);
            }
        });

        const docToUpdate = {updateOp: -testId, shardKey: -testId};
        testCase.commands.push({
            cmdObj: {
                update: collName,
                updates: [{q: docToUpdate, u: {$mul: {updateOp: 100}}}],
                stmtId: NumberInt(++testCase.lastUsedStmtId)
            },
            checkResponseFunc: (res) => {
                assert.eq(res.n, 1);
                assert.eq(res.nModified, 1);
            }
        });
        const updatedDoc = {updateOp: -testId * 100, shardKey: -testId};

        const docToDelete = {deleteOp: -testId, shardKey: -testId};
        testCase.commands.push({
            cmdObj: {
                delete: collName,
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
            const coll = st.s.getDB(dbName).getCollection(collName);
            assert.commandWorked(coll.insert(docToUpdate));
            assert.commandWorked(coll.insert(docToDelete));
        };

        testCase.checkDocsFunc = (isTxnCommitted) => {
            const coll = st.s.getDB(dbName).getCollection(collName);
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

    function makeTransactionOptionsForFindAndModifyTest(
        dbName, collName, {imageType, isPreparedTxn, isLargeTxn, abortOnInitialTry}) {
        const testId = ++lastTestId;

        const testCase = makeSessionOptionsForTest(testId);
        testCase.id = testId;
        testCase.dbName = dbName;
        testCase.collName = collName;
        testCase.isPreparedTxn = isPreparedTxn;
        testCase.isLargeTxn = isLargeTxn;
        testCase.abortOnInitialTry = abortOnInitialTry;
        testCase.imageType = imageType;
        testCase.lastUsedStmtId = -1;

        testCase.commands = [];

        // Define the retryable findAndModify statement to be run in the test internal transaction.
        // Prior to migration0, the write statement will be routed to shard0.
        const docToUpdate = {findAndModifyOp: -testId, shardKey: -testId};
        const findAndModifyCmdObj = {
            findAndModify: collName,
            query: docToUpdate,
            update: {$mul: {findAndModifyOp: 100}},
            stmtId: NumberInt(++testCase.lastUsedStmtId)
        };
        if (imageType == kImageType.kPostImage) {
            findAndModifyCmdObj.new = true;
        }
        testCase.commands.push({
            cmdObj: findAndModifyCmdObj,
            checkResponseFunc: (res) => {
                assert.eq(res.lastErrorObject.n, 1);
                if (imageType == kImageType.kNone) {
                    assert.eq(res.lastErrorObject.updatedExisting, false);
                } else {
                    assert.eq(res.lastErrorObject.updatedExisting, true);
                    delete res.value._id;
                    assert.eq(res.value,
                              imageType == kImageType.kPreImage ? docToUpdate : updatedDoc);
                }
            }
        });
        const updatedDoc = {
            findAndModifyOp: -testId * 100,
            shardKey: -testId,
        };

        // If testing a prepared and/or large transaction, define additional insert statements to
        // make the transaction a prepared and/or large transaction.
        const docsToInsert =
            makeInsertCommandsIfTestingPreparedOrLargeTransaction(testId, testCase);

        testCase.setUpFunc = () => {
            const coll = st.s.getDB(dbName).getCollection(collName);
            assert.commandWorked(coll.insert(docToUpdate));
        };

        testCase.checkDocsFunc = (isTxnCommitted) => {
            const coll = st.s.getDB(dbName).getCollection(collName);
            const docs = coll.find().toArray();

            assert.eq(coll.find(docToUpdate).itcount(), isTxnCommitted ? 0 : 1, tojson(docs));
            assert.eq(coll.find(updatedDoc).itcount(), isTxnCommitted ? 1 : 0, tojson(docs));

            docsToInsert.forEach(docToInsert => {
                assert.eq(coll.find(docToInsert).itcount(), isTxnCommitted ? 1 : 0, tojson(docs));
            });
        };

        return testCase;
    }

    function makeInsertCommandsIfTestingPreparedOrLargeTransaction(testId, testCase) {
        const additionalDocsToInsert = [];
        if (testCase.isLargeTxn) {
            // Prior to migration0, the insert statements below will be routed to shard0.
            const numLargeDocs = 2;
            for (let i = 0; i < numLargeDocs; i++) {
                const docToInsert = {
                    insert: i.toString(),
                    shardKey: -testId,
                };
                testCase.commands.push({
                    // Use stmtId -1 to get test coverage for "applyOps" entries without a stmtId.
                    cmdObj: {
                        insert: testCase.collName,
                        documents: [docToInsert],
                        stmtId: NumberInt(-1)
                    },
                    checkResponseFunc: (res) => {
                        assert.eq(res.n, 1);
                    }
                });
                additionalDocsToInsert.push(docToInsert);
            }
        }
        if (testCase.isPreparedTxn) {
            // Prior to migration0, this insert statement below will be routed to shard1.
            const docToInsert = {insertOp: testId, shardKey: testId};
            testCase.commands.push({
                cmdObj: {
                    insert: testCase.collName,
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
        assert.commandWorked(st.s.adminCommand(makeCommitTransactionCmdObj(lsid, txnNumber)));
    }

    function abortTransaction(lsid, txnNumber, isPreparedTxn) {
        if (isPreparedTxn) {
            const shard0Primary = st.rs0.getPrimary();
            assert.commandWorked(
                shard0Primary.adminCommand(makePrepareTransactionCmdObj(lsid, txnNumber)));
        }
        assert.commandWorked(st.s.adminCommand(makeAbortTransactionCmdObj(lsid, txnNumber)));
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

    function assertNoConfigTxnEntryOnRecipient(migration, lsid) {
        const configTxnEntries = getTxnEntriesForSession(migration.recipientRst, lsid);
        assert.eq(configTxnEntries, 0, tojson(configTxnEntries));
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
        runTxnRetryOnTransientError(() => {
            const txnNumber = getNextTxnNumber(txnType, testCase);

            for (let i = 0; i < testCase.commands.length; i++) {
                const command = testCase.commands[i];
                const cmdObj =
                    Object.assign({}, command.cmdObj, {lsid, txnNumber, autocommit: false});
                if (i == 0) {
                    cmdObj.startTransaction = true;
                }
                const res = assert.commandWorked(st.s.getDB(testCase.dbName).runCommand(cmdObj));
                command.checkResponseFunc(res);
            }
            if (testCase.abortOnInitialTry) {
                abortTransaction(lsid, txnNumber, testCase.isPreparedTxn);
            } else {
                commitTransaction(lsid, txnNumber);
            }
        });

        testCase.checkDocsFunc(!testCase.abortOnInitialTry /* isTxnCommitted */);
    }

    /*
     * Retries the commands defined inside 'testCase' in an internal transaction of the specified
     * type.
     */
    function runInternalTransactionOnRetry(txnType, testCase, isRetryAfterAbort) {
        jsTest.log(
            `Retrying write statements executed in an internal transaction with options ${tojson({
                id: testCase.id,
                isPreparedTxn: testCase.isPreparedTxn,
                isLargeTxn: testCase.isLargeTxn,
                abortOnInitialTry: testCase.abortOnInitialTry,
                imageType: testCase.imageType,
                isRetryAfterAbort: isRetryAfterAbort,
            })} in another internal transaction`);

        const lsid = getTransactionSessionId(txnType, testCase);
        // Give the session a different txnUUID to simulate a retry from a different mongos.
        lsid.txnUUID = UUID();
        runTxnRetryOnTransientError(() => {
            const txnNumber = getNextTxnNumber(txnType, testCase);

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
                const res = assert.commandWorked(st.s.getDB(testCase.dbName).runCommand(cmdObj));
                command.checkResponseFunc(res);
            }
            commitTransaction(lsid, txnNumber);
        });

        testCase.checkDocsFunc(true /* isTxnCommitted */);
    }

    /*
     * Retries the commands specified inside 'testCase' as retryable writes.
     */
    function runRetryableWriteOnRetry(testCase) {
        jsTest.log(
            `Retrying write statements executed in an internal transaction with options ${tojson({
                id: testCase.id,
                isPreparedTxn: testCase.isPreparedTxn,
                isLargeTxn: testCase.isLargeTxn,
                abortOnInitialTry: testCase.abortOnInitialTry,
                imageType: testCase.imageType,
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
            const res = assert.commandWorked(st.s.getDB(testCase.dbName).runCommand(cmdObj));
            command.checkResponseFunc(res);
            assert.commandWorked(res);
        }

        testCase.checkDocsFunc(true /* isTxnCommitted */);
    }

    /*
     * Below are the steps in this test:
     * 1. Run migration0, i.e. migrate chunk [MinKey, 1] from shard0 to shard2. Pause the migration
     *    while it is in the transfer mods phase.
     * 2. Run the commands specified in each 'testCase' inside an internal transaction of the
     *    specified type.
     * 3. Unpause migration0 and wait for it to complete.
     * 4. Retry writes in an internal transaction and retryable write.
     * 5. Restart shard2.
     * 6. Retry writes in an internal transaction and retryable write.
     * 7. Run migration1, i.e. migrate chunk [MinKey, 1] from shard2 to shard1.
     * 8. Retry writes in an internal transaction and retryable write.
     *
     * - If the type is non-retryable, verifies that none of the transactions has a
     *   config.transactions entry on the recipient after migration0 and returns early after step 4.
     * - If the type is retryable, verifies that the retryable write statements in each transaction
     *   are retryable on the recipient after each migration completes regardless of whether the
     *   transaction committed or aborted on the donor. For each transaction that aborts on the
     *   initial try, additionally verifies that the transaction does not have a config.transactions
     *   entry on the recipient after migration0.
     */
    function testTransactionsDuringChunkMigration(txnType, migrationOpts, testCases) {
        runCommandDuringTransferMods(st.s,
                                     staticMongod,
                                     migrationOpts.ns,
                                     migrationOpts.migration0.cmdObj.find,
                                     null,
                                     migrationOpts.migration0.donorShard,
                                     migrationOpts.migration0.recipientShard,
                                     () => {
                                         for (let testCase of testCases) {
                                             runInternalTransactionOnInitialTry(txnType, testCase);
                                         }
                                     });

        if (txnType == kInternalTxnType.kNonRetryable) {
            for (let testCase of testCases) {
                assertNoConfigTxnEntryOnRecipient(migrationOpts.migration0,
                                                  testCase.childLsidForNonRetryableWrite);
            }
            return;
        }

        for (let testCase of testCases) {
            if (testCase.abortOnInitialTry) {
                assertNoConfigTxnEntryOnRecipient(migrationOpts.migration0, testCase.parentLsid);
                assertNoConfigTxnEntryOnRecipient(migrationOpts.migration0,
                                                  testCase.childLsidForRetryableWrite);
            }
        }

        jsTest.log("Start retrying retryable internal transactions after migration0");
        for (let testCase of testCases) {
            runInternalTransactionOnRetry(
                txnType, testCase, testCase.abortOnInitialTry /* isRetryAfterAbort */);
            // Also retry the write statements as retryable writes.
            runRetryableWriteOnRetry(testCase);
        }
        jsTest.log("Finished retrying retryable internal transactions after migration0");

        migrationOpts.migration0.recipientRst.stopSet(null /* signal */, true /*forRestart */);
        migrationOpts.migration0.recipientRst.startSet({restart: true});
        migrationOpts.migration0.recipientRst.getPrimary();

        jsTest.log("Start retrying retryable internal transactions after restarting the " +
                   "recipient after migration0");
        for (let testCase of testCases) {
            runInternalTransactionOnRetry(txnType, testCase, false /* isRetryAfterAbort */);
            // Also retry the write statements as retryable writes.
            runRetryableWriteOnRetry(testCase);
        }
        jsTest.log("Finished retrying retryable internal transactions after restarting the " +
                   "recipient after migration0");

        assert.commandWorked(st.s.adminCommand(migrationOpts.migration1.cmdObj));

        jsTest.log("Start retrying retryable internal transactions after migration1");
        for (let testCase of testCases) {
            runInternalTransactionOnRetry(txnType, testCase, false /* isRetryAfterAbort */);
            // Also retry the write statements as retryable writes.
            runRetryableWriteOnRetry(testCase);
        }
        jsTest.log("Finished retrying retryable internal transactions after migration1");
    }

    /*
     * Below are the steps in this test:
     * 1. Runs the commands specified in each 'testCase' inside an internal transaction of the
     *    specified type.
     * 2. Run migration0, i.e. migrate chunk [MinKey, 1] from shard0 to shard2.
     * 3. Retry writes in an internal transaction and retryable write.
     * 4. Restart shard2.
     * 5. Retry writes in an internal transaction and retryable write.
     * 6. Run migration1, i.e. migrate chunk [MinKey, 1] from shard2 to shard1.
     * 7. Retry writes in an internal transaction and retryable write.
     *
     * - If the type is non-retryable, verifies that none of the transactions has a
     *   config.transactions entry on the recipient after migration0 and returns early after step 3.
     * - If the type is retryable, verifies that the retryable write statements in each transaction
     *   are retryable on the recipient after each migration completes regardless of whether the
     *   transaction committed or aborted on the donor. For each transaction that aborts on the
     *   initial try, additionally verifies that the transaction does not have a config.transactions
     *   entry on the recipient after migration0.
     */
    function testTransactionsBeforeChunkMigration(txnType, migrationOpts, testCases) {
        jsTest.log("Start running retryable internal transactions before migration0");
        for (let testCase of testCases) {
            runInternalTransactionOnInitialTry(txnType, testCase);
        }
        jsTest.log("Finished running retryable internal transactions before migration0");

        assert.commandWorked(st.s.adminCommand(migrationOpts.migration0.cmdObj));

        if (txnType == kInternalTxnType.kNonRetryable) {
            for (let testCase of testCases) {
                assertNoConfigTxnEntryOnRecipient(migrationOpts.migration0,
                                                  testCase.childLsidForNonRetryableWrite);
            }
            return;
        }

        for (let testCase of testCases) {
            if (testCase.abortOnInitialTry) {
                assertNoConfigTxnEntryOnRecipient(migrationOpts.migration0, testCase.parentLsid);
                assertNoConfigTxnEntryOnRecipient(migrationOpts.migration0,
                                                  testCase.childLsidForRetryableWrite);
            }
        }

        jsTest.log("Start retrying retryable internal transactions after migration0");
        for (let testCase of testCases) {
            runInternalTransactionOnRetry(
                txnType, testCase, testCase.abortOnInitialTry /* isRetryAfterAbort */);
            // Also retry the write statements as retryable writes.
            runRetryableWriteOnRetry(testCase);
        }
        jsTest.log("Finished retrying retryable internal transactions after migration0");

        migrationOpts.migration0.recipientRst.stopSet(null /* signal */, true /*forRestart */);
        migrationOpts.migration0.recipientRst.startSet({restart: true});
        migrationOpts.migration0.recipientRst.getPrimary();

        jsTest.log("Start retrying retryable internal transactions after restarting the " +
                   "recipient after migration0");
        for (let testCase of testCases) {
            runInternalTransactionOnRetry(txnType, testCase, false /* isRetryAfterAbort */);
            // Also retry the write statements as retryable writes.
            runRetryableWriteOnRetry(testCase);
        }
        jsTest.log("Finished retrying retryable internal transactions after restarting the " +
                   "recipient after migration0");

        assert.commandWorked(st.s.adminCommand(migrationOpts.migration1.cmdObj));

        jsTest.log("Start retrying retryable internal transactions after migration1");
        for (let testCase of testCases) {
            runInternalTransactionOnRetry(txnType, testCase, false /* isRetryAfterAbort */);
            // Also retry the write statements as retryable writes.
            runRetryableWriteOnRetry(testCase);
        }
        jsTest.log("Finished retrying retryable internal transactions after migration1");
    }

    this.InternalTxnType = kInternalTxnType;

    // Helpers for testing retryable internal transactions with chunk migration. Each helper defines
    // transaction test cases using the makeTransactionOptions*() helpers above and runs one
    // of the test*ChunkMigration() helpers above.

    function runTestForInsertUpdateDelete(txnType, testFunc, abortOnInitialTry) {
        const migrationOpts = setUpTestCollection();
        const testCases = [];
        for (let isPreparedTxn of [true, false]) {
            for (let isLargeTxn of [true, false]) {
                testCases.push(makeTransactionOptionsForInsertUpdateDeleteTest(
                    migrationOpts.dbName,
                    migrationOpts.collName,
                    {isPreparedTxn, isLargeTxn, abortOnInitialTry}));
            }
        }
        testFunc(txnType, migrationOpts, testCases);
    }

    function runTestForFindAndModify(txnType, testFunc, abortOnInitialTry) {
        const migrationOpts = setUpTestCollection();
        const testCases = [];
        for (let imageTypeName in kImageType) {
            const imageType = kImageType[imageTypeName];
            for (let isPreparedTxn of [true, false]) {
                for (let isLargeTxn of [true, false]) {
                    testCases.push(makeTransactionOptionsForFindAndModifyTest(
                        migrationOpts.dbName,
                        migrationOpts.collName,
                        {isPreparedTxn, isLargeTxn, abortOnInitialTry, imageType}));
                }
            }
        }
        testFunc(txnType, migrationOpts, testCases);
    }

    this.runTestForInsertUpdateDeleteBeforeChunkMigration = function(txnType, abortOnInitialTry) {
        runTestForInsertUpdateDelete(
            txnType, testTransactionsBeforeChunkMigration, abortOnInitialTry);
    };

    this.runTestForInsertUpdateDeleteDuringChunkMigration = function(txnType, abortOnInitialTry) {
        runTestForInsertUpdateDelete(
            txnType, testTransactionsDuringChunkMigration, abortOnInitialTry);
    };

    this.runTestForFindAndModifyBeforeChunkMigration = function(txnType, abortOnInitialTry) {
        runTestForFindAndModify(txnType, testTransactionsBeforeChunkMigration, abortOnInitialTry);
    };

    this.runTestForFindAndModifyDuringChunkMigration = function(txnType, abortOnInitialTry) {
        runTestForFindAndModify(txnType, testTransactionsDuringChunkMigration, abortOnInitialTry);
    };

    this.stop = function() {
        st.stop();
        MongoRunner.stopMongod(staticMongod);
    };
}
