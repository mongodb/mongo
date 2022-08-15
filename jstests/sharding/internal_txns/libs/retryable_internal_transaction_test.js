/*
 * Utilities for testing that internal transactions for retryable writes can be retried.
 */
'use strict';

load('jstests/sharding/internal_txns/libs/fixture_helpers.js');
load('jstests/sharding/libs/sharded_transactions_helpers.js');

function getOplogEntriesForTxnWithRetries(rs, lsid, txnNumber) {
    let oplogEntries;
    assert.soon(
        () => {
            try {
                oplogEntries = getOplogEntriesForTxn(rs, lsid, txnNumber);
                return true;
            } catch (e) {
                // This read from the oplog can fail with CappedPositionLost if the oplog is
                // concurrently truncated, but the test should only need the oplog entries
                // from a recent transaction, which shouldn't be truncated because of the
                // increased oplog size, so it should be safe to retry on this error.
                if (e.code !== ErrorCodes.CappedPositionLost) {
                    throw e;
                }
                print("Retrying loading oplog entries on CappedPositionLost error: " + tojson(e));
            }
        },
        () => {
            return "Failed to get oplog entries for transaction, lsid: " + tojson(lsid) +
                ", txnNumber: " + tojson(txnNumber) +
                ", latest oplog entries: " + tojson(oplogEntries);
        });
    return oplogEntries;
}

function RetryableInternalTransactionTest(collectionOptions = {}) {
    // Transactions with more than two operations will have the behavior of large transactions and
    // span multiple oplog entries.
    const maxNumberOfTransactionOperationsInSingleOplogEntry = 2;

    // Set a large oplogSize since this test runs a find command to get the oplog entries for
    // every transaction that it runs including large transactions and with the default oplogSize,
    // oplog reading done by the find command may not be able to keep up with the oplog truncation,
    // causing the command to fail with CappedPositionLost.
    const st = new ShardingTest({
        shards: 1,
        rs: {nodes: 2, oplogSize: 256},
        rsOptions: {
            setParameter: {
                maxNumberOfTransactionOperationsInSingleOplogEntry:
                    maxNumberOfTransactionOperationsInSingleOplogEntry
            }
        }
    });

    const kTestMode = {kNonRecovery: 1, kRestart: 2, kFailover: 3, kRollback: 4};

    // Used when testing large transactions (i.e. in 'testRetryLargeTxn') for specifying which
    // applyOps oplog entry should contain the entry for retryable write being tested.
    // 'testRetryLargeTxn' runs a large transaction with three applyOps oplog entries.
    const kOplogEntryLocation = {kFirst: 1, kMiddle: 2, kLast: 3};

    const kDbName = "testDb";
    const kCollName = "testColl";
    const mongosTestDB = st.s.getDB(kDbName);
    assert.commandWorked(mongosTestDB.createCollection(kCollName, collectionOptions));
    const mongosTestColl = mongosTestDB.getCollection(kCollName);

    function makeSessionIdForRetryableInternalTransaction() {
        return {id: UUID(), txnNumber: NumberLong(0), txnUUID: UUID()};
    }

    function setTxnFields(cmdObj, lsid, txnNumber) {
        cmdObj.lsid = lsid;
        cmdObj.txnNumber = NumberLong(txnNumber);
        cmdObj.autocommit = false;
    }

    const getRandomOplogEntryLocation = function() {
        const locations = Object.values(kOplogEntryLocation);
        return locations[Math.floor(Math.random() * locations.length)];
    };

    function getTransactionState(lsid, txnNumber) {
        return {
            oplogEntries: getOplogEntriesForTxnWithRetries(st.rs0, lsid, txnNumber),
            txnEntries: getTxnEntriesForSession(st.rs0, lsid),
            imageEntries: getImageEntriesForTxn(st.rs0, lsid, txnNumber)
        };
    }

    function assertConsistentImageEntries(lsid, txnNumber) {
        const imageEntriesOnPrimary =
            getImageEntriesForTxnOnNode(st.rs0.getPrimary(), lsid, txnNumber);
        st.rs0.getSecondaries().forEach(secondary => {
            const imageEntriesOnSecondary = getImageEntriesForTxnOnNode(secondary, lsid, txnNumber);
            assert.eq(imageEntriesOnSecondary, imageEntriesOnPrimary);
        });
    }

    function setUpTestMode(mode) {
        if (mode == kTestMode.kRestart) {
            st.rs0.restart(0, {
                remember: true,
                startClean: false,
            });
            const newPrimary = st.rs0.getPrimary();
        } else if (mode == kTestMode.kFailover) {
            const oldPrimary = st.rs0.getPrimary();
            assert.commandWorked(
                oldPrimary.adminCommand({replSetStepDown: ReplSetTest.kForeverSecs, force: true}));
            assert.commandWorked(oldPrimary.adminCommand({replSetFreeze: 0}));
            const newPrimary = st.rs0.getPrimary();
        }
    }

    function commitTransaction(lsid, txnNumber, isPreparedTxn, isRetry) {
        const commitCmdObj = makeCommitTransactionCmdObj(lsid, txnNumber);
        if (isPreparedTxn && !isRetry) {
            const shard0Primary = st.rs0.getPrimary();
            const prepareCmdObj = makePrepareTransactionCmdObj(lsid, txnNumber);
            const prepareRes = assert.commandWorked(shard0Primary.adminCommand(prepareCmdObj));
            commitCmdObj.commitTimestamp = prepareRes.prepareTimestamp;
            assert.commandWorked(shard0Primary.adminCommand(commitCmdObj));
        }
        assert.commandWorked(mongosTestDB.adminCommand(commitCmdObj));
    }

    function testNonRetryableBasic(cmdObj, {
        txnOptions,
        testMode,
        expectFindAndModifyImageInOplog,
        expectFindAndModifyImageInSideCollection
    }) {
        // A findAndModify write statement in a non-retryable transaction will not generate a
        // pre/post image.
        assert(!expectFindAndModifyImageInOplog);
        assert(!expectFindAndModifyImageInSideCollection);
        jsTest.log("Testing retrying a non-retryable internal transaction");
        cmdObj.startTransaction = true;

        // Initial try.
        const initialLsid = txnOptions.makeSessionIdFunc();
        let initialTxnNumber = 0;
        runTxnRetryOnTransientError(() => {
            initialTxnNumber++;
            setTxnFields(cmdObj, initialLsid, initialTxnNumber);
            assert.commandWorked(mongosTestDB.runCommand(cmdObj));
            commitTransaction(initialLsid, initialTxnNumber, txnOptions.isPreparedTxn);
        });

        const initialTxnStateBefore = getTransactionState(initialLsid, initialTxnNumber);
        assert.eq(initialTxnStateBefore.oplogEntries.length,
                  (txnOptions.isPreparedTxn ? 2 : 1) + (expectFindAndModifyImageInOplog ? 1 : 0),
                  initialTxnStateBefore.oplogEntries);
        assert.eq(initialTxnStateBefore.imageEntries.length,
                  expectFindAndModifyImageInSideCollection ? 1 : 0,
                  initialTxnStateBefore.imageEntries);
        assertConsistentImageEntries(initialLsid, initialTxnNumber);

        setUpTestMode(testMode);

        // Retry.
        assert.commandFailedWithCode(mongosTestDB.runCommand(cmdObj),
                                     ErrorCodes.ConflictingOperationInProgress);

        const initialTxnStateAfter = getTransactionState(initialLsid, initialTxnNumber);
        assert.eq(initialTxnStateBefore.oplogEntries, initialTxnStateAfter.oplogEntries);
        assert.eq(initialTxnStateBefore.txnEntries, initialTxnStateAfter.txnEntries);
        assert.eq(initialTxnStateBefore.imageEntries, initialTxnStateAfter.imageEntries);

        assert.commandWorked(mongosTestColl.remove({}));
    }

    function testRetryableBasic(cmdObj, {
        txnOptions,
        testMode,
        expectFindAndModifyImageInOplog,
        expectFindAndModifyImageInSideCollection,
        checkRetryResponseFunc
    }) {
        assert(!expectFindAndModifyImageInOplog || !expectFindAndModifyImageInSideCollection);
        jsTest.log(
            "Testing retrying a retryable internal transaction with one applyOps oplog entry");
        cmdObj.startTransaction = true;

        // Initial try.
        const initialLsid = txnOptions.makeSessionIdFunc();
        let initialTxnNumber = 0;
        let initialRes;
        runTxnRetryOnTransientError(() => {
            initialTxnNumber++;
            setTxnFields(cmdObj, initialLsid, initialTxnNumber);
            initialRes = assert.commandWorked(mongosTestDB.runCommand(cmdObj));
            commitTransaction(initialLsid, initialTxnNumber, txnOptions.isPreparedTxn);
        });

        const initialTxnStateBefore = getTransactionState(initialLsid, initialTxnNumber);
        assert.eq(initialTxnStateBefore.oplogEntries.length,
                  (txnOptions.isPreparedTxn ? 2 : 1) + (expectFindAndModifyImageInOplog ? 1 : 0),
                  initialTxnStateBefore.oplogEntries);
        assert.eq(initialTxnStateBefore.imageEntries.length,
                  expectFindAndModifyImageInSideCollection ? 1 : 0,
                  initialTxnStateBefore.imageEntries);
        assertConsistentImageEntries(initialLsid, initialTxnNumber);

        setUpTestMode(testMode);

        // Retry in the initial internal transaction. No need to commit since the transaction has
        // already committed.
        const retryRes = assert.commandWorked(mongosTestDB.runCommand(cmdObj));
        checkRetryResponseFunc(initialRes, retryRes);

        // Retry in a different internal transaction (running in an internal session with a
        // different txnUUID) to simulate a retry from a different mongos.
        const retryLsid = Object.assign({}, initialLsid, {txnUUID: UUID()});
        let retryTxnNumber = 0;
        runTxnRetryOnTransientError(() => {
            retryTxnNumber++;
            setTxnFields(cmdObj, retryLsid, retryTxnNumber);
            const retryRes = assert.commandWorked(mongosTestDB.runCommand(cmdObj));
            checkRetryResponseFunc(initialRes, retryRes);
            commitTransaction(
                retryLsid, retryTxnNumber, txnOptions.isPreparedTxn, true /* isRetry */);
        });

        const initialTxnStateAfter = getTransactionState(initialLsid, initialTxnNumber);
        assert.eq(initialTxnStateBefore.oplogEntries, initialTxnStateAfter.oplogEntries);
        assert.eq(initialTxnStateBefore.txnEntries, initialTxnStateAfter.txnEntries);
        assert.eq(initialTxnStateBefore.imageEntries, initialTxnStateAfter.imageEntries);
        // The retry should not generate any persisted transaction state.
        const retryTxnState = getTransactionState(retryLsid, retryTxnNumber);
        assert.eq(retryTxnState.oplogEntries.length, 0, retryTxnState);
        assert.eq(retryTxnState.txnEntries.length, 0, retryTxnState);
        assert.eq(retryTxnState.imageEntries.length, 0, retryTxnState);

        assert.commandWorked(mongosTestColl.remove({}));
    }

    function testRetryableLargeTxn(cmdObj, {
        txnOptions,
        testMode,
        expectFindAndModifyImageInOplog,
        expectFindAndModifyImageInSideCollection,
        checkRetryResponseFunc
    }) {
        assert(!expectFindAndModifyImageInOplog || !expectFindAndModifyImageInSideCollection);

        jsTest.log(
            "Testing retrying a retryable internal transaction with more than one applyOps oplog entry");

        let stmtId = 1;
        let makeInsertCmdObj = (docs) => {
            assert.eq(maxNumberOfTransactionOperationsInSingleOplogEntry, docs.length);
            return {
                insert: kCollName,
                documents: docs,
                stmtIds: [NumberInt(stmtId++), NumberInt(stmtId++)],
            };
        };
        let makeCmdObjToRetry = (cmdObj) => {
            const cmdObjToRetry = Object.assign(cmdObj, {
                stmtId: NumberInt(stmtId),
            });
            if (cmdObjToRetry.documents) {
                stmtId += cmdObjToRetry.documents.length;
            } else if (cmdObjToRetry.updates) {
                stmtId += cmdObjToRetry.updates.length;
            } else if (cmdObjToRetry.deletes) {
                stmtId += cmdObjToRetry.deletes.length;
            } else {
                stmtId++;
            }
            return cmdObjToRetry;
        };

        const insertCmdObj0 = makeInsertCmdObj([{_id: -100, x: 100}, {_id: -101, x: 101}]);
        const insertCmdObj1 = makeInsertCmdObj([{_id: -200, x: 200}, {_id: -201, x: 201}]);
        const insertCmdObj2 = makeInsertCmdObj([{_id: -300, x: 300}, {_id: -301, x: 301}]);
        const cmdObjToRetry = makeCmdObjToRetry(cmdObj);
        const insertCmdObjs = [insertCmdObj0, insertCmdObj1, insertCmdObj2];

        // Initial try.
        const initialLsid = txnOptions.makeSessionIdFunc();
        let initialTxnNumber = 0;
        let initialRes;
        runTxnRetryOnTransientError(() => {
            initialTxnNumber++;
            setTxnFields(cmdObjToRetry, initialLsid, initialTxnNumber);
            insertCmdObjs.forEach(cmdObj => setTxnFields(cmdObj, initialLsid, initialTxnNumber));
            if (txnOptions.oplogEntryLocation == kOplogEntryLocation.kLast) {
                assert.commandWorked(mongosTestDB.runCommand(
                    Object.assign(insertCmdObj0, {startTransaction: true})));
                assert.commandWorked(mongosTestDB.runCommand(insertCmdObj1));
                assert.commandWorked(mongosTestDB.runCommand(insertCmdObj2));
                initialRes = assert.commandWorked(mongosTestDB.runCommand(cmdObjToRetry));
            } else if (txnOptions.oplogEntryLocation == kOplogEntryLocation.kMiddle) {
                assert.commandWorked(mongosTestDB.runCommand(
                    Object.assign(insertCmdObj0, {startTransaction: true})));
                assert.commandWorked(mongosTestDB.runCommand(insertCmdObj1));
                initialRes = assert.commandWorked(mongosTestDB.runCommand(cmdObjToRetry));
                assert.commandWorked(mongosTestDB.runCommand(insertCmdObj2));
            } else {
                initialRes = assert.commandWorked(mongosTestDB.runCommand(
                    Object.assign({}, cmdObjToRetry, {startTransaction: true})));
                assert.commandWorked(mongosTestDB.runCommand(insertCmdObj0));
                assert.commandWorked(mongosTestDB.runCommand(insertCmdObj1));
                assert.commandWorked(mongosTestDB.runCommand(insertCmdObj2));
            }
            commitTransaction(initialLsid, initialTxnNumber, txnOptions.isPreparedTxn);
        });

        const initialTxnStateBefore = getTransactionState(initialLsid, initialTxnNumber);

        // stmtId is one greater than the total number of statements executed in the transaction.
        const expectedOplogLength =
            Math.floor(stmtId / maxNumberOfTransactionOperationsInSingleOplogEntry);
        assert.eq(initialTxnStateBefore.oplogEntries.length,
                  (txnOptions.isPreparedTxn ? expectedOplogLength + 1 : expectedOplogLength) +
                      (expectFindAndModifyImageInOplog ? 1 : 0));
        assert.eq(initialTxnStateBefore.imageEntries.length,
                  expectFindAndModifyImageInSideCollection ? 1 : 0,
                  initialTxnStateBefore.imageEntries);
        assertConsistentImageEntries(initialLsid, initialTxnNumber);

        setUpTestMode(testMode);

        // Retry in the initial internal transaction. No need to commit since the transaction has
        // already committed.
        const retryRes = assert.commandWorked(mongosTestDB.runCommand(cmdObj));
        checkRetryResponseFunc(initialRes, retryRes);

        // Retry in a different internal transaction (running in an internal session with a
        // different txnUUID) to simulate a retry from a different mongos.
        const retryLsid = Object.assign({}, initialLsid, {txnUUID: UUID()});
        let retryTxnNumber = 0;
        runTxnRetryOnTransientError(() => {
            retryTxnNumber++;
            setTxnFields(cmdObjToRetry, retryLsid, retryTxnNumber);
            insertCmdObj0.startTransaction = true;
            insertCmdObjs.forEach(cmdObj => setTxnFields(cmdObj, retryLsid, retryTxnNumber));
            insertCmdObjs.forEach(insertCmdObj => {
                const retryRes = assert.commandWorked(mongosTestDB.runCommand(insertCmdObj));
                assert.eq(retryRes.n, 2);
            });
            const retryRes = assert.commandWorked(mongosTestDB.runCommand(cmdObjToRetry));
            checkRetryResponseFunc(initialRes, retryRes);
            commitTransaction(
                retryLsid, retryTxnNumber, txnOptions.isPreparedTxn, true /* isRetry */);
        });

        const initialTxnStateAfter = getTransactionState(initialLsid, initialTxnNumber);
        assert.eq(initialTxnStateBefore.oplogEntries, initialTxnStateAfter.oplogEntries);
        assert.eq(initialTxnStateBefore.txnEntries, initialTxnStateAfter.txnEntries);
        assert.eq(initialTxnStateBefore.imageEntries, initialTxnStateAfter.imageEntries);
        // The retry should not generate any persisted transaction state.
        const retryTxnState = getTransactionState(retryLsid, retryTxnNumber);
        assert.eq(retryTxnState.oplogEntries.length, 0, retryTxnState);
        assert.eq(retryTxnState.txnEntries.length, 0, retryTxnState);
        assert.eq(retryTxnState.imageEntries.length, 0, retryTxnState);

        assert.commandWorked(mongosTestColl.remove({}));
    }

    function testRetry(cmdObj, {
        txnOptions,
        testMode,
        expectRetryToSucceed,
        expectFindAndModifyImageInOplog,
        expectFindAndModifyImageInSideCollection,
        checkRetryResponseFunc
    }) {
        const testRetryFunc = (() => {
            if (txnOptions.isLargeTxn) {
                // This fixture only supports testing large retryable transactions since when a
                // non-retryable transaction is retried, it fails before the it even starts so
                // testing with a large transaction doesn't add any test coverage.
                assert(expectRetryToSucceed);
                return testRetryableLargeTxn;
            }
            return expectRetryToSucceed ? testRetryableBasic : testNonRetryableBasic;
        })();
        testRetryFunc(cmdObj, {
            txnOptions,
            testMode,
            expectRetryToSucceed,
            expectFindAndModifyImageInOplog,
            expectFindAndModifyImageInSideCollection,
            checkRetryResponseFunc
        });
    }

    function testRetryInserts({txnOptions, testMode, expectRetryToSucceed}) {
        jsTest.log("Testing batched inserts");

        const insertCmdObj = {
            insert: kCollName,
            documents: [{_id: 0, x: 0}, {_id: 1, x: 1}],
        };
        const checkRetryResponseFunc = (initialRes, retryRes) => {
            assert.eq(initialRes.n, retryRes.n);
            insertCmdObj.documents.forEach(doc => {
                assert.eq(mongosTestColl.count(doc), 1);
            });
        };
        testRetry(insertCmdObj,
                  {txnOptions, testMode, expectRetryToSucceed, checkRetryResponseFunc});
    }

    function testRetryUpdates({txnOptions, testMode, expectRetryToSucceed}) {
        jsTest.log("Testing batched updates");

        assert.commandWorked(mongosTestColl.insert([{_id: 0, x: 0}, {_id: 1, x: 1}]));

        const updateCmdObj = {
            update: kCollName,
            updates:
                [{q: {_id: 0, x: 0}, u: {$inc: {x: 10}}}, {q: {_id: 1, x: 1}, u: {$inc: {x: 10}}}],
        };
        const checkRetryResponseFunc = (initialRes, retryRes) => {
            assert.eq(initialRes.nModified, retryRes.nModified);
            updateCmdObj.updates.forEach(updateArgs => {
                const originalDoc = updateArgs.q;
                const updatedDoc = Object.assign({}, updateArgs.q);
                updatedDoc.x += updateArgs.u.$inc.x;
                assert.eq(mongosTestColl.count(originalDoc), 0);
                assert.eq(mongosTestColl.count(updatedDoc), 1);
            });
        };
        testRetry(updateCmdObj,
                  {txnOptions, testMode, expectRetryToSucceed, checkRetryResponseFunc});
    }

    function testRetryDeletes({txnOptions, testMode, expectRetryToSucceed}) {
        jsTest.log("Testing batched deletes");

        assert.commandWorked(mongosTestColl.insert([{_id: 0, x: 0}, {_id: 1, x: 1}]));

        const deleteCmdObj = {
            delete: kCollName,
            deletes: [{q: {_id: 0, x: 0}, limit: 1}, {q: {_id: 1, x: 1}, limit: 1}],
        };
        const checkRetryResponseFunc = (initialRes, retryRes) => {
            assert.eq(initialRes.n, retryRes.n);
            deleteCmdObj.deletes.forEach(deleteArgs => {
                assert.eq(mongosTestColl.count(deleteArgs.q), 0);
            });
        };
        testRetry(deleteCmdObj,
                  {txnOptions, testMode, expectRetryToSucceed, checkRetryResponseFunc});
    }

    function testRetryFindAndModify(findAndModifyCmdObj, {
        txnOptions,
        testMode,
        enableFindAndModifyImageCollection,
        expectRetryToSucceed,
        expectFindAndModifyImage,
    }) {
        const shard0Primary = st.rs0.getPrimary();
        assert.commandWorked(shard0Primary.adminCommand({
            setParameter: 1,
            storeFindAndModifyImagesInSideCollection: enableFindAndModifyImageCollection
        }));
        const checkRetryResponseFunc = (initialRes, retryRes) => {
            assert.eq(initialRes.lastErrorObject, retryRes.lastErrorObject);
            assert.eq(initialRes.value, retryRes.value);
        };

        testRetry(findAndModifyCmdObj, {
            txnOptions,
            testMode,
            expectRetryToSucceed,
            expectFindAndModifyImageInOplog: expectRetryToSucceed && expectFindAndModifyImage &&
                !enableFindAndModifyImageCollection,
            expectFindAndModifyImageInSideCollection: expectRetryToSucceed &&
                expectFindAndModifyImage && enableFindAndModifyImageCollection,
            checkRetryResponseFunc
        });
    }

    function testRetryFindAndModifyUpsert({
        txnOptions,
        testMode,
        enableFindAndModifyImageCollection,
        expectRetryToSucceed,
    }) {
        jsTest.log(
            "Testing findAndModify upsert (i.e. no preImage or postImage) with enableFindAndModifyImageCollection: " +
            enableFindAndModifyImageCollection);

        const findAndModifyCmdObj = {
            findAndModify: kCollName,
            query: {_id: -1, x: -1},
            update: {$inc: {x: -10}},
            upsert: true,
        };
        const expectFindAndModifyImage = false;  // no pre or post image.
        testRetryFindAndModify(findAndModifyCmdObj, {
            txnOptions,
            testMode,
            enableFindAndModifyImageCollection,
            expectFindAndModifyImage,
            expectRetryToSucceed,
        });
    }

    function testRetryFindAndModifyUpdateWithPreImage({
        txnOptions,
        testMode,
        enableFindAndModifyImageCollection,
        expectRetryToSucceed,
    }) {
        jsTest.log(
            "Testing findAndModify update with preImage with enableFindAndModifyImageCollection: " +
            enableFindAndModifyImageCollection);

        assert.commandWorked(mongosTestColl.insert([{_id: -1, x: -1}]));
        const findAndModifyCmdObj = {
            findAndModify: kCollName,
            query: {_id: -1, x: -1},
            update: {$inc: {x: -10}},
        };
        const expectFindAndModifyImage = true;
        testRetryFindAndModify(findAndModifyCmdObj, {
            txnOptions,
            testMode,
            enableFindAndModifyImageCollection,
            expectFindAndModifyImage,
            expectRetryToSucceed,
        });
    }

    function testRetryFindAndModifyUpdateWithPostImage({
        txnOptions,
        testMode,
        enableFindAndModifyImageCollection,
        expectRetryToSucceed,
    }) {
        jsTest.log(
            "Testing findAndModify update with postImage with enableFindAndModifyImageCollection: " +
            enableFindAndModifyImageCollection);

        assert.commandWorked(mongosTestColl.insert([{_id: -1, x: -1}]));
        const findAndModifyCmdObj = {
            findAndModify: kCollName,
            query: {_id: -1, x: -1},
            update: {$inc: {x: -10}},
            new: true,
        };
        const expectFindAndModifyImage = true;
        testRetryFindAndModify(findAndModifyCmdObj, {
            txnOptions,
            testMode,
            enableFindAndModifyImageCollection,
            expectFindAndModifyImage,
            expectRetryToSucceed,
        });
    }

    function testRetryFindAndModifyRemove({
        txnOptions,
        testMode,
        enableFindAndModifyImageCollection,
        expectRetryToSucceed,
    }) {
        jsTest.log(
            "Testing findAndModify remove (i.e. with preImage) with enableFindAndModifyImageCollection: " +
            enableFindAndModifyImageCollection);

        assert.commandWorked(mongosTestColl.insert([{_id: -1, x: -1}]));
        const findAndModifyCmdObj = {
            findAndModify: kCollName,
            query: {_id: -1, x: -1},
            remove: true,
        };
        const expectFindAndModifyImage = true;
        testRetryFindAndModify(findAndModifyCmdObj, {
            txnOptions,
            testMode,
            enableFindAndModifyImageCollection,
            expectFindAndModifyImage,
            expectRetryToSucceed,
        });
    }

    this.TestMode = kTestMode;

    this.runInsertUpdateDeleteTests = function(testOptions) {
        if (testOptions.txnOptions.isLargeTxn) {
            testOptions.txnOptions.oplogEntryLocation = getRandomOplogEntryLocation();
        }
        jsTest.log(`Testing insert, update and delete with options: ${tojson(testOptions)}`);

        testRetryInserts(testOptions);
        testRetryUpdates(testOptions);
        testRetryDeletes(testOptions);
    };

    function runFindAndModifyTests(testOptions) {
        if (testOptions.txnOptions.isLargeTxn) {
            testOptions.txnOptions.oplogEntryLocation = getRandomOplogEntryLocation();
        }
        jsTest.log(`Testing findAndModify with options: ${tojson(testOptions)}`);

        testRetryFindAndModifyUpsert(testOptions);
        testRetryFindAndModifyUpdateWithPreImage(testOptions);
        testRetryFindAndModifyUpdateWithPostImage(testOptions);
        testRetryFindAndModifyRemove(testOptions);
    }

    this.runFindAndModifyTestsEnableImageCollection = function(testOptions) {
        testOptions.enableFindAndModifyImageCollection = true;
        runFindAndModifyTests(testOptions);
    };

    this.runTestsForAllUnpreparedRetryableInternalTransactionTypes = function(runTestsFunc,
                                                                              testMode) {
        const makeSessionIdFunc = makeSessionIdForRetryableInternalTransaction;
        const expectRetryToSucceed = true;

        runTestsFunc({
            txnOptions: {makeSessionIdFunc, isPreparedTxn: false, isLargeTxn: false},
            testMode,
            expectRetryToSucceed
        });

        runTestsFunc({
            txnOptions: {makeSessionIdFunc, isPreparedTxn: false, isLargeTxn: true},
            testMode,
            expectRetryToSucceed
        });
    };

    this.runTestsForAllPreparedRetryableInternalTransactionTypes = function(runTestsFunc,
                                                                            testMode) {
        const makeSessionIdFunc = makeSessionIdForRetryableInternalTransaction;
        const expectRetryToSucceed = true;

        runTestsFunc({
            txnOptions: {makeSessionIdFunc, isPreparedTxn: true, isLargeTxn: false},
            testMode,
            expectRetryToSucceed
        });

        runTestsFunc({
            txnOptions: {makeSessionIdFunc, isPreparedTxn: true, isLargeTxn: true},
            testMode,
            expectRetryToSucceed
        });
    };

    this.runTestsForAllRetryableInternalTransactionTypes = function(runTestsFunc, testMode) {
        this.runTestsForAllUnpreparedRetryableInternalTransactionTypes(runTestsFunc, testMode);
        this.runTestsForAllPreparedRetryableInternalTransactionTypes(runTestsFunc, testMode);
    };

    this.testRetryableTxnMultiWrites = function testRetryableTxnMultiWrites() {
        assert.commandWorked(
            mongosTestColl.insert([{_id: 0, x: 0}, {_id: 1, x: 0}, {_id: 2, x: 0}]));

        const lsid = {id: UUID(), txnUUID: UUID(), txnNumber: NumberLong(21)};
        let txnNumber = 1;

        //
        // Test multi updates.
        //

        // Updates with an initialized stmtId should fail.
        const retryableUpdateCmd = {
            update: kCollName,
            updates: [{q: {x: 0}, u: {$inc: {x: 10}}, multi: true}],
            lsid,
            txnNumber: NumberLong(txnNumber),
            startTransaction: true,
            autocommit: false,
        };
        const retryableUpdateRes = mongosTestDB.runCommand(retryableUpdateCmd);
        assert(retryableUpdateRes.hasOwnProperty("writeErrors"));
        assert.commandFailedWithCode(retryableUpdateRes, ErrorCodes.InvalidOptions);

        assert.commandFailedWithCode(
            mongosTestDB.adminCommand(makeAbortTransactionCmdObj(lsid, txnNumber)),
            ErrorCodes.NoSuchTransaction);

        // The documents shouldn't have been updated.
        assert.eq(3, mongosTestColl.find({x: 0}).itcount());

        // Updates with the uninitialized stmtId should succeed.
        txnNumber++;
        const nonRetryableUpdateCmd = {
            update: kCollName,
            updates: [{q: {x: 0}, u: {$inc: {x: 10}}, multi: true}],
            lsid,
            txnNumber: NumberLong(txnNumber),
            stmtId: NumberInt(-1),
            startTransaction: true,
            autocommit: false,
        };
        assert.commandWorked(mongosTestDB.runCommand(nonRetryableUpdateCmd));

        assert.commandWorked(
            mongosTestDB.adminCommand(makeCommitTransactionCmdObj(lsid, txnNumber)));

        // The documents should have been updated.
        assert.eq(0, mongosTestColl.find({x: 0}).itcount());
        assert.eq(3, mongosTestColl.find({x: 10}).itcount());

        assert.commandWorked(mongosTestColl.remove({}));
        assert.commandWorked(
            mongosTestColl.insert([{_id: 0, x: 0}, {_id: 1, x: 0}, {_id: 2, x: 0}]));

        //
        // Test multi deletes.
        //

        // Deletes with an initialized stmtId should fail.
        txnNumber++;
        const retryableDeleteCmd = {
            delete: kCollName,
            deletes: [{q: {x: 0}, limit: 0}],
            lsid,
            txnNumber: NumberLong(txnNumber),
            startTransaction: true,
            autocommit: false,
        };
        const retryableDeleteRes = mongosTestDB.runCommand(retryableDeleteCmd);
        assert(retryableDeleteRes.hasOwnProperty("writeErrors"));
        assert.commandFailedWithCode(retryableDeleteRes, ErrorCodes.InvalidOptions);

        assert.commandFailedWithCode(
            mongosTestDB.adminCommand(makeAbortTransactionCmdObj(lsid, txnNumber)),
            ErrorCodes.NoSuchTransaction);

        // The documents shouldn't have been deleted.
        assert.eq(3, mongosTestColl.find({x: 0}).itcount());

        // Deletes with the uninitialized stmtId should succeed.
        txnNumber++;
        const nonRetryableDeleteCmd = {
            delete: kCollName,
            deletes: [{q: {x: 0}, limit: 0}],
            lsid,
            txnNumber: NumberLong(txnNumber),
            stmtId: NumberInt(-1),
            startTransaction: true,
            autocommit: false,
        };
        assert.commandWorked(mongosTestDB.runCommand(nonRetryableDeleteCmd));

        assert.commandWorked(
            mongosTestDB.adminCommand(makeCommitTransactionCmdObj(lsid, txnNumber)));

        // The documents should have been deleted.
        assert.eq(0, mongosTestColl.find().itcount());
    };

    this.testNonRetryableTxnMultiWrites = function testNonRetryableTxnMultiWrites() {
        assert.commandWorked(
            mongosTestColl.insert([{_id: 0, x: 0}, {_id: 1, x: 0}, {_id: 2, x: 0}]));

        const lsid = {id: UUID(), txnUUID: UUID()};
        let txnNumber = 1;

        //
        // Test multi updates.
        //

        const updateCmd = {
            update: kCollName,
            updates: [{q: {x: 0}, u: {$inc: {x: 10}}, multi: true}],
            lsid,
            txnNumber: NumberLong(txnNumber),
            startTransaction: true,
            autocommit: false,
        };
        assert.commandWorked(mongosTestDB.runCommand(updateCmd));
        assert.commandWorked(
            mongosTestDB.adminCommand(makeCommitTransactionCmdObj(lsid, txnNumber)));

        // The documents should have been updated.
        assert.eq(0, mongosTestColl.find({x: 0}).itcount());
        assert.eq(3, mongosTestColl.find({x: 10}).itcount());

        //
        // Test multi deletes.
        //

        assert.commandWorked(mongosTestColl.remove({}));
        assert.commandWorked(
            mongosTestColl.insert([{_id: 0, x: 0}, {_id: 1, x: 0}, {_id: 2, x: 0}]));

        txnNumber++;
        const deleteCmd = {
            delete: kCollName,
            deletes: [{q: {x: 0}, limit: 0}],
            lsid,
            txnNumber: NumberLong(txnNumber),
            startTransaction: true,
            autocommit: false,
        };
        assert.commandWorked(mongosTestDB.runCommand(deleteCmd));
        assert.commandWorked(
            mongosTestDB.adminCommand(makeCommitTransactionCmdObj(lsid, txnNumber)));

        // The documents should have been deleted.
        assert.eq(0, mongosTestColl.find().itcount());
    };

    this.stop = function() {
        st.stop();
    };
}
