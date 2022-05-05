/*
 * Utilities for testing that internal transactions for retryable writes can be retried.
 */
'use strict';

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
    // Set a large oplogSize since this test runs a find command to get the oplog entries for
    // every transaction that it runs including large transactions and with the default oplogSize,
    // oplog reading done by the find command may not be able to keep up with the oplog truncation,
    // causing the command to fail with CappedPositionLost.
    const st = new ShardingTest({shards: 1, rs: {nodes: 2, oplogSize: 256}});

    const kTestMode = {kNonRecovery: 1, kRestart: 2, kFailover: 3, kRollback: 4};

    // Used when testing large transactions (i.e. in 'testRetryLargeTxn') for specifying which
    // applyOps oplog entry should contain the entry for retryable write being tested.
    // 'testRetryLargeTxn' runs a large transaction with three applyOps oplog entries.
    const kOplogEntryLocation = {kFirst: 1, kMiddle: 2, kLast: 3};

    // For creating documents that will result in large transactions.
    const kSize10MB = 10 * 1024 * 1024;

    const kDbName = "testDb";
    const kCollName = "testColl";
    const mongosTestDB = st.s.getDB(kDbName);
    assert.commandWorked(mongosTestDB.createCollection(kCollName, collectionOptions));
    const mongosTestColl = mongosTestDB.getCollection(kCollName);

    function makeSessionIdForRetryableInternalTransaction() {
        return {id: UUID(), txnNumber: NumberLong(0), txnUUID: UUID()};
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
            st.rs0.stopSet(null /* signal */, true /*forRestart */);
            st.rs0.startSet({restart: true});
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

    function testRetryBasic(cmdObj, lsid, txnNumber, {
        expectRetryToSucceed,
        expectFindAndModifyImageInOplog,
        expectFindAndModifyImageInSideCollection,
        txnOptions,
        testMode,
        checkFunc
    }) {
        assert(!expectFindAndModifyImageInOplog || !expectFindAndModifyImageInSideCollection);

        const cmdObjToRetry = Object.assign(cmdObj, {
            lsid: lsid,
            txnNumber: NumberLong(txnNumber),
            startTransaction: true,
            autocommit: false,
        });

        const initialRes = assert.commandWorked(mongosTestDB.runCommand(cmdObjToRetry));
        commitTransaction(lsid, txnNumber, txnOptions.isPreparedTxn);

        const txnStateBeforeRetry = getTransactionState(lsid, txnNumber);
        assert.eq(txnStateBeforeRetry.oplogEntries.length,
                  (txnOptions.isPreparedTxn ? 2 : 1) + (expectFindAndModifyImageInOplog ? 1 : 0),
                  txnStateBeforeRetry.oplogEntries);
        assert.eq(txnStateBeforeRetry.imageEntries.length,
                  expectFindAndModifyImageInSideCollection ? 1 : 0,
                  txnStateBeforeRetry.imageEntries);
        assertConsistentImageEntries(lsid, txnNumber);

        setUpTestMode(testMode);

        const retryRes = mongosTestDB.runCommand(cmdObjToRetry);
        if (expectRetryToSucceed) {
            assert.commandWorked(retryRes);
            checkFunc(initialRes, retryRes);
            commitTransaction(lsid, txnNumber, txnOptions.isPreparedTxn, true /* isRetry */);
        } else {
            assert.commandFailedWithCode(retryRes, ErrorCodes.ConflictingOperationInProgress);
        }

        const txnStateAfterRetry = getTransactionState(lsid, txnNumber);
        assert.eq(txnStateBeforeRetry.oplogEntries, txnStateAfterRetry.oplogEntries);
        assert.eq(txnStateBeforeRetry.txnEntries, txnStateAfterRetry.txnEntries);
        assert.eq(txnStateBeforeRetry.imageEntries, txnStateAfterRetry.imageEntries);

        assert.commandWorked(mongosTestColl.remove({}));
    }

    function testRetryLargeTxn(cmdObj, lsid, txnNumber, {
        expectFindAndModifyImageInOplog,
        expectFindAndModifyImageInSideCollection,
        txnOptions,
        testMode,
        checkFunc
    }) {
        assert(!expectFindAndModifyImageInOplog || !expectFindAndModifyImageInSideCollection);

        jsTest.log(
            "Testing retrying a retryable internal transaction with more than one applyOps oplog entry");

        let stmtId = 1;
        let makeInsertCmdObj = (doc) => {
            return {
                insert: kCollName,
                documents: [Object.assign(doc, {y: new Array(kSize10MB).join("a")})],
                lsid: lsid,
                txnNumber: NumberLong(txnNumber),
                stmtId: NumberInt(stmtId++),
                autocommit: false
            };
        };
        let makeCmdObjToRetry = (cmdObj) => {
            const cmdObjToRetry = Object.assign(cmdObj, {
                lsid: lsid,
                txnNumber: NumberLong(txnNumber),
                stmtId: NumberInt(stmtId),
                autocommit: false,
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

        const insertCmdObj0 =
            Object.assign(makeInsertCmdObj({_id: -100, x: 100}), {startTransaction: true});
        const cmdObjToRetry = makeCmdObjToRetry(cmdObj);
        const insertCmdObj1 = makeInsertCmdObj({_id: -200, x: -200});
        const insertCmdObj2 = makeInsertCmdObj({_id: -300, x: -300});
        const insertCmdObjs = [insertCmdObj0, insertCmdObj1, insertCmdObj2];

        let initialRes;
        if (txnOptions.oplogEntryLocation == kOplogEntryLocation.kLast) {
            assert.commandWorked(mongosTestDB.runCommand(insertCmdObj0));
            assert.commandWorked(mongosTestDB.runCommand(insertCmdObj1));
            assert.commandWorked(mongosTestDB.runCommand(insertCmdObj2));
            initialRes = assert.commandWorked(mongosTestDB.runCommand(cmdObjToRetry));
        } else if (txnOptions.oplogEntryLocation == kOplogEntryLocation.kMiddle) {
            assert.commandWorked(mongosTestDB.runCommand(insertCmdObj0));
            assert.commandWorked(mongosTestDB.runCommand(insertCmdObj1));
            initialRes = assert.commandWorked(mongosTestDB.runCommand(cmdObjToRetry));
            assert.commandWorked(mongosTestDB.runCommand(insertCmdObj2));
        } else {
            assert.commandWorked(mongosTestDB.runCommand(insertCmdObj0));
            initialRes = assert.commandWorked(mongosTestDB.runCommand(cmdObjToRetry));
            assert.commandWorked(mongosTestDB.runCommand(insertCmdObj1));
            assert.commandWorked(mongosTestDB.runCommand(insertCmdObj2));
        }
        commitTransaction(lsid, txnNumber, txnOptions.isPreparedTxn);

        const txnStateBeforeRetry = getTransactionState(lsid, txnNumber);
        assert.eq(txnStateBeforeRetry.oplogEntries.length,
                  (txnOptions.isPreparedTxn ? insertCmdObjs.length + 1 : insertCmdObjs.length) +
                      (expectFindAndModifyImageInOplog ? 1 : 0));
        assert.eq(txnStateBeforeRetry.imageEntries.length,
                  expectFindAndModifyImageInSideCollection ? 1 : 0,
                  txnStateBeforeRetry.imageEntries);
        assertConsistentImageEntries(lsid, txnNumber);

        setUpTestMode(testMode);

        insertCmdObjs.forEach(insertCmdObj => {
            const retryRes = assert.commandWorked(mongosTestDB.runCommand(insertCmdObj));
            assert.eq(retryRes.n, 1);
        });
        const retryRes = assert.commandWorked(mongosTestDB.runCommand(cmdObjToRetry));
        checkFunc(initialRes, retryRes);
        commitTransaction(lsid, txnNumber, txnOptions.isPreparedTxn, true /* isRetry */);

        const txnStateAfterRetry = getTransactionState(lsid, txnNumber);
        assert.eq(txnStateBeforeRetry.oplogEntries, txnStateAfterRetry.oplogEntries);
        assert.eq(txnStateBeforeRetry.txnEntries, txnStateAfterRetry.txnEntries);
        assert.eq(txnStateBeforeRetry.imageEntries, txnStateBeforeRetry.imageEntries);

        assert.commandWorked(mongosTestColl.remove({}));
    }

    function testRetry(cmdObj, lsid, txnNumber, {
        expectRetryToSucceed,
        expectFindAndModifyImageInOplog,
        expectFindAndModifyImageInSideCollection,
        txnOptions,
        testMode,
        checkFunc
    }) {
        const testRetryFunc = txnOptions.isLargeTxn ? testRetryLargeTxn : testRetryBasic;
        testRetryFunc(cmdObj, lsid, txnNumber, {
            expectRetryToSucceed,
            expectFindAndModifyImageInOplog,
            expectFindAndModifyImageInSideCollection,
            txnOptions,
            testMode,
            checkFunc
        });
    }

    function testRetryInserts(lsid, txnNumber, {expectRetryToSucceed, txnOptions, testMode}) {
        jsTest.log("Testing batched inserts");

        const insertCmdObj = {
            insert: kCollName,
            documents: [{_id: 0, x: 0}, {_id: 1, x: 1}],
        };
        const checkFunc = (initialRes, retryRes) => {
            assert.eq(initialRes.n, retryRes.n);
            insertCmdObj.documents.forEach(doc => {
                assert.eq(mongosTestColl.count(doc), 1);
            });
        };
        testRetry(
            insertCmdObj, lsid, txnNumber, {expectRetryToSucceed, txnOptions, testMode, checkFunc});
    }

    function testRetryUpdates(lsid, txnNumber, {expectRetryToSucceed, txnOptions, testMode}) {
        jsTest.log("Testing batched updates");

        assert.commandWorked(mongosTestColl.insert([{_id: 0, x: 0}, {_id: 1, x: 1}]));

        const updateCmdObj = {
            update: kCollName,
            updates:
                [{q: {_id: 0, x: 0}, u: {$inc: {x: 10}}}, {q: {_id: 1, x: 1}, u: {$inc: {x: 10}}}],
        };
        const checkFunc = (initialRes, retryRes) => {
            assert.eq(initialRes.nModified, retryRes.nModified);
            updateCmdObj.updates.forEach(updateArgs => {
                const originalDoc = updateArgs.q;
                const updatedDoc = Object.assign({}, updateArgs.q);
                updatedDoc.x += updateArgs.u.$inc.x;
                assert.eq(mongosTestColl.count(originalDoc), 0);
                assert.eq(mongosTestColl.count(updatedDoc), 1);
            });
        };
        testRetry(
            updateCmdObj, lsid, txnNumber, {expectRetryToSucceed, txnOptions, testMode, checkFunc});
    }

    function testRetryDeletes(lsid, txnNumber, {expectRetryToSucceed, txnOptions, testMode}) {
        jsTest.log("Testing batched deletes");

        assert.commandWorked(mongosTestColl.insert([{_id: 0, x: 0}, {_id: 1, x: 1}]));

        const deleteCmdObj = {
            delete: kCollName,
            deletes: [{q: {_id: 0, x: 0}, limit: 1}, {q: {_id: 1, x: 1}, limit: 1}],
        };
        const checkFunc = (initialRes, retryRes) => {
            assert.eq(initialRes.n, retryRes.n);
            deleteCmdObj.deletes.forEach(deleteArgs => {
                assert.eq(mongosTestColl.count(deleteArgs.q), 0);
            });
        };
        testRetry(
            deleteCmdObj, lsid, txnNumber, {expectRetryToSucceed, txnOptions, testMode, checkFunc});
    }

    function testRetryFindAndModify(findAndModifyCmdObj, lsid, txnNumber, {
        expectRetryToSucceed,
        expectFindAndModifyImage,
        txnOptions,
        testMode,
        enableFindAndModifyImageCollection,
    }) {
        const shard0Primary = st.rs0.getPrimary();
        assert.commandWorked(shard0Primary.adminCommand({
            setParameter: 1,
            storeFindAndModifyImagesInSideCollection: enableFindAndModifyImageCollection
        }));
        const checkFunc = (initialRes, retryRes) => {
            assert.eq(initialRes.lastErrorObject, retryRes.lastErrorObject);
            assert.eq(initialRes.value, retryRes.value);
        };

        testRetry(findAndModifyCmdObj, lsid, txnNumber, {
            expectRetryToSucceed,
            expectFindAndModifyImageInOplog: expectRetryToSucceed && expectFindAndModifyImage &&
                !enableFindAndModifyImageCollection,
            expectFindAndModifyImageInSideCollection: expectRetryToSucceed &&
                expectFindAndModifyImage && enableFindAndModifyImageCollection,
            txnOptions,
            testMode,
            checkFunc
        });
    }

    function testRetryFindAndModifyUpsert(lsid, txnNumber, {
        expectRetryToSucceed,
        txnOptions,
        testMode,
        enableFindAndModifyImageCollection,
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
        testRetryFindAndModify(findAndModifyCmdObj, lsid, txnNumber, {
            expectRetryToSucceed,
            expectFindAndModifyImage,
            txnOptions,
            testMode,
            enableFindAndModifyImageCollection,
        });
    }

    function testRetryFindAndModifyUpdateWithPreImage(lsid, txnNumber, {
        expectRetryToSucceed,
        txnOptions,
        testMode,
        enableFindAndModifyImageCollection,
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
        testRetryFindAndModify(findAndModifyCmdObj, lsid, txnNumber, {
            expectRetryToSucceed,
            expectFindAndModifyImage,
            txnOptions,
            testMode,
            enableFindAndModifyImageCollection,
        });
    }

    function testRetryFindAndModifyUpdateWithPostImage(lsid, txnNumber, {
        expectRetryToSucceed,
        txnOptions,
        testMode,
        enableFindAndModifyImageCollection,
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
        testRetryFindAndModify(findAndModifyCmdObj, lsid, txnNumber, {
            expectRetryToSucceed,
            expectFindAndModifyImage,
            txnOptions,
            testMode,
            enableFindAndModifyImageCollection,
        });
    }

    function testRetryFindAndModifyRemove(lsid, txnNumber, {
        expectRetryToSucceed,
        txnOptions,
        testMode,
        enableFindAndModifyImageCollection,
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
        testRetryFindAndModify(findAndModifyCmdObj, lsid, txnNumber, {
            expectRetryToSucceed,
            enableFindAndModifyImageCollection,
            txnOptions,
            testMode,
            expectFindAndModifyImage,
        });
    }

    this.TestMode = kTestMode;

    this.runInsertUpdateDeleteTests = function(lsid, testOptions) {
        testOptions.lastUsedTxnNumber =
            testOptions.lastUsedTxnNumber ? testOptions.lastUsedTxnNumber : 0;
        testOptions.txnOptions = testOptions.txnOptions ? testOptions.txnOptions : {};
        if (testOptions.txnOptions.isLargeTxn) {
            testOptions.txnOptions.oplogEntryLocation = getRandomOplogEntryLocation();
        }
        jsTest.log(`Testing insert, update and delete with options: ${tojson(testOptions)}`);

        testRetryInserts(lsid, testOptions.lastUsedTxnNumber++, testOptions);
        testRetryUpdates(lsid, testOptions.lastUsedTxnNumber++, testOptions);
        testRetryDeletes(lsid, testOptions.lastUsedTxnNumber++, testOptions);
    };

    function runFindAndModifyTests(lsid, testOptions) {
        testOptions.lastUsedTxnNumber =
            testOptions.lastUsedTxnNumber ? testOptions.lastUsedTxnNumber : 0;
        testOptions.txnOptions = testOptions.txnOptions ? testOptions.txnOptions : {};
        if (testOptions.txnOptions.isLargeTxn) {
            testOptions.txnOptions.oplogEntryLocation = getRandomOplogEntryLocation();
        }
        jsTest.log(`Testing findAndModify with options: ${tojson(testOptions)}`);

        testRetryFindAndModifyUpsert(lsid, testOptions.lastUsedTxnNumber++, testOptions);
        testRetryFindAndModifyUpdateWithPreImage(
            lsid, testOptions.lastUsedTxnNumber++, testOptions);
        testRetryFindAndModifyUpdateWithPostImage(
            lsid, testOptions.lastUsedTxnNumber++, testOptions);
        testRetryFindAndModifyRemove(lsid, testOptions.lastUsedTxnNumber++, testOptions);
    }

    this.runFindAndModifyTestsEnableImageCollection = function(lsid, testOptions) {
        testOptions.enableFindAndModifyImageCollection = true;
        runFindAndModifyTests(lsid, testOptions);
    };

    this.runFindAndModifyTestsDisableImageCollection = function(lsid, testOptions) {
        testOptions.enableFindAndModifyImageCollection = false;
        runFindAndModifyTests(lsid, testOptions);
    };

    this.runTestsForAllUnpreparedRetryableInternalTransactionTypes = function(runTestsFunc,
                                                                              testMode) {
        const expectRetryToSucceed = true;

        runTestsFunc(makeSessionIdForRetryableInternalTransaction(), {
            expectRetryToSucceed,
            txnOptions: {isPreparedTxn: false, isLargeTxn: false},
            testMode
        });

        runTestsFunc(
            makeSessionIdForRetryableInternalTransaction(),
            {expectRetryToSucceed, txnOptions: {isPreparedTxn: false, isLargeTxn: true}, testMode});
    };

    this.runTestsForAllPreparedRetryableInternalTransactionTypes = function(runTestsFunc,
                                                                            testMode) {
        const expectRetryToSucceed = true;

        runTestsFunc(
            makeSessionIdForRetryableInternalTransaction(),
            {expectRetryToSucceed, txnOptions: {isPreparedTxn: true, isLargeTxn: false}, testMode});

        runTestsFunc(
            makeSessionIdForRetryableInternalTransaction(),
            {expectRetryToSucceed, txnOptions: {isPreparedTxn: true, isLargeTxn: true}, testMode});
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
