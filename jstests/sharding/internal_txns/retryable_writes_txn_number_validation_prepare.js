/*
 * Verifies transaction participants correctly detect prepared child transactions when validating
 * transaction numbers for incoming transactions.
 *
 * @tags: [requires_fcv_60, uses_transactions, requires_persistence]
 */
(function() {
'use strict';

load("jstests/libs/fail_point_util.js");
load("jstests/libs/parallelTester.js");
load("jstests/libs/uuid_util.js");
load('jstests/sharding/libs/sharded_transactions_helpers.js');

const rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();
let primary = rst.getPrimary();

const dbName = "testDb";
const collName = "testColl";
let testDB = primary.getDB(dbName);
let testColl = testDB.getCollection(collName);

assert.commandWorked(testDB.createCollection(collName));

const kTestMode = {
    kNonRecovery: 1,
    kRestart: 2,
    kFailoverOldPrimary: 3,
    kFailoverNewPrimary: 4,
};

function setUpTestMode(mode) {
    if (mode == kTestMode.kRestart) {
        rst.stopSet(null /* signal */,
                    true /*forRestart */,
                    {skipValidation: true, skipCheckDBHashes: true});
        rst.startSet({restart: true});
        primary = rst.getPrimary();
    } else if (mode == kTestMode.kFailoverOldPrimary) {
        const oldPrimary = rst.getPrimary();
        const oldSecondary = rst.getSecondary();

        assert.commandWorked(oldSecondary.adminCommand({replSetFreeze: ReplSetTest.kForeverSecs}));
        assert.commandWorked(
            oldPrimary.adminCommand({replSetStepDown: ReplSetTest.kForeverSecs, force: true}));
        assert.commandWorked(oldPrimary.adminCommand({replSetFreeze: 0}));

        const newPrimary = rst.getPrimary();
        assert.eq(oldPrimary, newPrimary);
        primary = newPrimary;
    } else if (mode == kTestMode.kFailoverNewPrimary) {
        const oldPrimary = rst.getPrimary();
        const oldSecondary = rst.getSecondary();

        assert.commandWorked(oldSecondary.adminCommand({replSetFreeze: 0}));
        assert.commandWorked(
            oldPrimary.adminCommand({replSetStepDown: ReplSetTest.kForeverSecs, force: true}));

        const newPrimary = rst.getPrimary();
        assert.neq(oldPrimary, newPrimary);
        primary = newPrimary;
    }
    testDB = primary.getDB(dbName);
    testColl = testDB.getCollection(collName);
}

function testTxnNumberValidationStartNewTxnNumberWhilePreviousIsInPrepare(
    testModeName, prevTxnNumberIsRetryableInternalTxn) {
    jsTest.log(`Testing with ${tojson({testModeName, prevTxnNumberIsRetryableInternalTxn})}`);
    const testMode = kTestMode[testModeName];
    const sessionUUID = UUID();

    let lsid, txnNumber, fpName;
    if (prevTxnNumberIsRetryableInternalTxn) {
        lsid = {id: sessionUUID, txnNumber: NumberLong(5), txnUUID: UUID()};
        txnNumber = NumberLong(0);
        fpName = "waitAfterNewStatementBlocksBehindOpenInternalTransactionForRetryableWrite";
    } else {
        lsid = {id: sessionUUID};
        txnNumber = NumberLong(5);
        fpName = "waitAfterNewStatementBlocksBehindPrepare";
    }

    const writeCmdObj = {
        insert: collName,
        documents: [{_id: 0}],
        lsid: lsid,
        txnNumber: NumberLong(txnNumber),
        startTransaction: true,
        autocommit: false
    };
    const prepareCmdObj = makePrepareTransactionCmdObj(lsid, txnNumber);
    const commitCmdObj = makeCommitTransactionCmdObj(lsid, txnNumber);

    assert.commandWorked(testDB.runCommand(writeCmdObj));
    const preparedTxnRes = assert.commandWorked(testDB.adminCommand(prepareCmdObj));
    commitCmdObj.commitTimestamp = preparedTxnRes.prepareTimestamp;

    setUpTestMode(testMode);
    rst.awaitLastOpCommitted();

    let runNewTxnNumber = function(primaryHost, parentSessionUUIDString, dbName, collName) {
        load('jstests/sharding/libs/sharded_transactions_helpers.js');

        const primary = new Mongo(primaryHost);
        const testDB = primary.getDB(dbName);

        const lsid = {id: UUID(parentSessionUUIDString), txnNumber: NumberLong(6), txnUUID: UUID()};
        const txnNumber = NumberLong(0);
        const writeCmdObj = {
            insert: collName,
            documents: [{_id: 1}],
            lsid: lsid,
            txnNumber: NumberLong(txnNumber),
            startTransaction: true,
            autocommit: false
        };
        const commitCmdObj = makeCommitTransactionCmdObj(lsid, txnNumber);

        assert.commandWorked(testDB.runCommand(writeCmdObj));
        assert.commandWorked(testDB.adminCommand(commitCmdObj));
    };

    const fp = configureFailPoint(primary, fpName);
    const newTxnNumberThread = new Thread(
        runNewTxnNumber, primary.host, extractUUIDFromObject(sessionUUID), dbName, collName);
    newTxnNumberThread.start();
    fp.wait();
    fp.off();

    assert.commandWorked(testDB.adminCommand(commitCmdObj));
    newTxnNumberThread.join();

    const docs = testColl.find().toArray();
    assert.eq(docs.length, 2, docs);
    assert.commandWorked(testColl.remove({}));
}

for (let testModeName in kTestMode) {
    for (let prevTxnNumberIsRetryableInternalTxn of [false, true]) {
        testTxnNumberValidationStartNewTxnNumberWhilePreviousIsInPrepare(
            testModeName, prevTxnNumberIsRetryableInternalTxn);
    }
}

rst.stopSet();
})();
