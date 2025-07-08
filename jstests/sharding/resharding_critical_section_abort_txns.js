/**
 * Tests that upon transitioning to "preparing-to-block-writes" state, resharding donors abort
 * unprepared transactions but not prepared transactions and that:
 * - The abort error code is InterruptedDueToReshardingCriticalSection.
 * - The response has RetryableWriteError label if the in-progress command is commitTransaction or
 *   abortTransaction. Otherwise, it has TransientTransactionError label.
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {ShardingTest} from 'jstests/libs/shardingtest.js';
import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";

function runMoveCollection(mongosHost, ns0, toShard) {
    const mongos = new Mongo(mongosHost);
    return mongos.adminCommand({moveCollection: ns0, toShard});
}

function makeInsertCmdObj(collName, lsid, txnNumber, docs) {
    return {
        insert: collName,
        documents: docs,
        lsid: lsid,
        txnNumber: NumberLong(txnNumber),
        startTransaction: true,
        autocommit: false
    };
}

function makePrepareTxnCmdObj(lsid, txnNumber) {
    return {
        prepareTransaction: 1,
        lsid: lsid,
        txnNumber: NumberLong(txnNumber),
        autocommit: false,
        writeConcern: {w: "majority"},
    };
}

function makeCommitTxnCmdObj(lsid, txnNumber, prepareRes) {
    let cmdObj = {
        commitTransaction: 1,
        lsid: lsid,
        txnNumber: NumberLong(txnNumber),
        autocommit: false,
        writeConcern: {w: "majority"},
    };
    if (prepareRes) {
        cmdObj.commitTimestamp = prepareRes.prepareTimestamp;
    }
    return cmdObj;
}

function makeAbortTxnCmdObj(lsid, txnNumber) {
    return {
        abortTransaction: 1,
        lsid: lsid,
        txnNumber: NumberLong(txnNumber),
        autocommit: false,
        writeConcern: {w: "majority"},
    };
}

function runInsertCmdInTxn(mongosHost, dbName, collName, lsidIdString, txnNumber, docs) {
    const mongos = new Mongo(mongosHost);
    const lsid = {id: UUID(lsidIdString)};
    const cmdObj = {
        insert: collName,
        documents: docs,
        lsid: lsid,
        txnNumber: NumberLong(txnNumber),
        startTransaction: true,
        autocommit: false
    };
    return mongos.getDB(dbName).runCommand(cmdObj);
}

function runUpdateCmdInTxn(mongosHost, dbName, collName, lsidIdString, txnNumber, updates) {
    const mongos = new Mongo(mongosHost);
    const lsid = {id: UUID(lsidIdString)};
    const cmdObj = {
        update: collName,
        updates,
        lsid: lsid,
        txnNumber: NumberLong(txnNumber),
        startTransaction: true,
        autocommit: false
    };
    return mongos.getDB(dbName).runCommand(cmdObj);
}

function runCommitTxnCmd(mongosHost, lsidIdString, txnNumber) {
    const mongos = new Mongo(mongosHost);
    const lsid = {id: UUID(lsidIdString)};
    const cmdObj = {
        commitTransaction: 1,
        lsid: lsid,
        txnNumber: NumberLong(txnNumber),
        autocommit: false,
        writeConcern: {w: "majority"},
    };
    return mongos.adminCommand(cmdObj);
}

function runAbortTxnCmd(mongosHost, lsidIdString, txnNumber) {
    const mongos = new Mongo(mongosHost);
    const lsid = {id: UUID(lsidIdString)};
    const cmdObj = {
        abortTransaction: 1,
        lsid: lsid,
        txnNumber: NumberLong(txnNumber),
        autocommit: false,
        writeConcern: {w: "majority"},
    };
    return mongos.adminCommand(cmdObj);
}

function assertInterruptedWithTransientTransactionErrorLabel(res) {
    assert.commandFailedWithCode(res, ErrorCodes.InterruptedDueToReshardingCriticalSection);
    assert(res.hasOwnProperty("errorLabels"), res);
    assert.eq(res.errorLabels, ["TransientTransactionError"], res);
}

function assertInterruptedWithRetryableWriteErrorLabel(res) {
    assert.commandFailedWithCode(res, ErrorCodes.InterruptedDueToReshardingCriticalSection);
    assert(res.hasOwnProperty("errorLabels"), res);
    assert.eq(res.errorLabels, ["RetryableWriteError"], res);
}

const st = new ShardingTest({shards: 2});
const configPrimary = st.configRS.getPrimary();
const shard1Primary = st.rs1.getPrimary();

let testNum = 0;
const dbName = "testDb";
const testDB = st.s.getDB(dbName);

const reshardingCriticalSectionTimeoutMillis = 5000;
assert.commandWorked(configPrimary.adminCommand({
    setParameter: 1,
    reshardingCriticalSectionTimeoutMillis,
}));

// Make shard0 the primary shard for the test database.
assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));

/**
 * Tests transitioning to "preparing-to-block-writes" state while there are unprepared transactions
 * that have checked the session back in.
 */
function testPreparingToBlockWritesWhileSessionCheckedIn(testOptions) {
    jsTest.log(
        "Testing preparing to block writes while the transaction has checked the session back in " +
        tojsononeline({testOptions}));
    testNum++;
    const collName0 = "testColl0_" + testNum;
    const collName1 = "testColl1_" + testNum;
    const ns0 = dbName + "." + collName0;
    const ns1 = dbName + "." + collName1;

    const testColl0 = testDB.getCollection(collName0);
    assert.commandWorked(testColl0.insert({x: 0}));

    const testColl1 = testDB.getCollection(collName1);
    assert.commandWorked(testColl1.insert({x: 0}));

    // By design, the primary shard is included as a recipient regardless of whether it is the shard
    // the collection is moving to. To make the moveCollection operation later in the test not have
    // a donor shard that also acts as a recipient, which would complicate the test, move
    // collection0 and also collection1 to shard1 (non-primary shard). That way, the moveCollection
    // operation will have shard1 as the donor and shard0 as the recipient.
    assert.commandWorked(st.s.adminCommand({moveCollection: ns0, toShard: st.shard1.shardName}));
    assert.commandWorked(st.s.adminCommand({moveCollection: ns1, toShard: st.shard1.shardName}));

    // Perform a find command against both collections after the moveCollection operations above so
    // that the writes in the transactions below do not need to refresh the sharding metadata which
    // can lead to ExceededTimeLimit errors on slow machines.
    assert.eq(testColl0.find().itcount(), 1);
    assert.eq(testColl1.find().itcount(), 1);

    let setParameterRes;
    if (!testOptions.enableAbortUnpreparedTxns) {
        // 'reshardingAbortUnpreparedTransactionsUponPreparingToBlockWrites' defaults to true. So
        // only set it when it needs to be set to false.
        setParameterRes = assert.commandWorked(shard1Primary.adminCommand({
            setParameter: 1,
            reshardingAbortUnpreparedTransactionsUponPreparingToBlockWrites: false,
        }));
    }

    let beforeBlockingFp =
        configureFailPoint(configPrimary, "reshardingPauseCoordinatorBeforeBlockingWrites");

    let moveCollThread = new Thread(runMoveCollection, st.s.host, ns0, st.shard0.shardName);
    moveCollThread.start();

    jsTest.log("Waiting for moveCollection to be about to enter the critical section");
    beforeBlockingFp.wait();

    jsTest.log("Starting a transaction on the donor shard involving the collection being moved");
    const lsid0 = {id: UUID()};
    const txnNumber0 = NumberLong(15);
    assert.commandWorked(
        testDB.runCommand(makeInsertCmdObj(collName0, lsid0, txnNumber0, [{x: 1}])));
    let prepareRes0;
    if (testOptions.preparedTxns) {
        prepareRes0 = assert.commandWorked(
            shard1Primary.adminCommand(makePrepareTxnCmdObj(lsid0, txnNumber0)));
    }

    jsTest.log("Starting a transaction on the donor shard involving the other collection");
    const lsid1 = {id: UUID()};
    const txnNumber1 = NumberLong(25);
    assert.commandWorked(
        testDB.runCommand(makeInsertCmdObj(collName1, lsid1, txnNumber1, [{x: 1}])));
    let prepareRes1;
    if (testOptions.preparedTxns) {
        prepareRes1 = assert.commandWorked(
            shard1Primary.adminCommand(makePrepareTxnCmdObj(lsid1, txnNumber1)));
    }

    jsTest.log("Unpausing moveCollection");
    beforeBlockingFp.off();

    if (testOptions.expectAbort) {
        // Upon transitioning to the "preparing-to-block-writes" state, shard1 should abort both
        // transactions, and the moveCollection operation should run to completion successfully.
        const moveCollRes = moveCollThread.returnData();
        assert.commandWorked(moveCollRes);

        assert.commandFailedWithCode(
            shard1Primary.adminCommand(makeCommitTxnCmdObj(lsid0, txnNumber0)),
            ErrorCodes.NoSuchTransaction);
        assert.commandFailedWithCode(
            shard1Primary.adminCommand(makeCommitTxnCmdObj(lsid1, txnNumber1)),
            ErrorCodes.NoSuchTransaction);

        assert.eq(testColl0.find({x: 1}).itcount(), 0);
        assert.eq(testColl1.find({x: 1}).itcount(), 0);

    } else {
        // Upon entering "preparing-to-block-writes" state, shard1 should not abort the prepared
        // transactions, instead it should get stuck waiting for them to complete until the critical
        // section timeout is reached.

        // TODO (SERVER-106862): Currently, upon aborting resharding, a donor would try to release
        // the critical section regardless of whether it had successfully acquired it. Releasing the
        // critical section involves taking an X lock on the collection being resharded. So in this
        // case, the lock acquisition or the abort itself is expected to block until we commit or
        // abort the transactions. After SERVER-106862, commit the transactions after joining the
        // moveCollection thread instead and assert that moveCollection command always fails with
        // ReshardingCriticalSectionTimeout.
        sleep(100);  // Sleep for a while to verify the transactions do not get aborted.

        jsTest.log("Verifying that neither of the transactions got aborted");
        assert.commandWorked(
            shard1Primary.adminCommand(makeCommitTxnCmdObj(lsid0, txnNumber0, prepareRes0)));
        assert.commandWorked(
            shard1Primary.adminCommand(makeCommitTxnCmdObj(lsid1, txnNumber1, prepareRes1)));

        const moveCollRes = moveCollThread.returnData();
        assert.commandWorkedOrFailedWithCode(moveCollRes,
                                             ErrorCodes.ReshardingCriticalSectionTimeout);

        assert.eq(testColl0.find({x: 1}).itcount(), 1);
        assert.eq(testColl1.find({x: 1}).itcount(), 1);
    }

    if (!testOptions.enableAbortUnpreparedTxns) {
        // Restore the original 'reshardingAbortUnpreparedTransactionsUponPreparingToBlockWrites'
        // value.
        setParameterRes = assert.commandWorked(shard1Primary.adminCommand({
            setParameter: 1,
            reshardingAbortUnpreparedTransactionsUponPreparingToBlockWrites: setParameterRes.was,
        }));
    }
}

/**
 * Tests transitioning to "preparing-to-block-writes" state while there are unprepared transactions
 * that have checked out the session to run commands other than commitTransaction and
 * abortTransaction. Sets 'reshardingAbortUnpreparedTransactionsUponPreparingToBlockWrites' to true.
 * Verifies that the donor aborts the transaction with InterruptedDueToReshardingCriticalSection
 * error code and the response has TransientTransactionError label.
 */
function testPreparingToBlockWritesWhileSessionCheckedOutNonCommitOrAbort() {
    jsTest.log("Testing preparing to block writes while the transaction has checked out the " +
               "session to run non-commitTransaction command");
    testNum++;
    const collName0 = "testColl0_" + testNum;
    const collName1 = "testColl1_" + testNum;
    const ns0 = dbName + "." + collName0;
    const ns1 = dbName + "." + collName1;

    const testColl0 = testDB.getCollection(collName0);
    assert.commandWorked(testColl0.insert({x: 0}));

    const testColl1 = testDB.getCollection(collName1);
    assert.commandWorked(testColl1.insert({x: 0}));

    // By design, the primary shard is included as a recipient regardless of whether it is the shard
    // the collection is moving to. To make the moveCollection operation later in the test not have
    // a donor shard that also acts as a recipient, which would complicate the test, move
    // collection0 and also collection1 to shard1 (non-primary shard). That way, the moveCollection
    // operation will have shard1 as the donor and shard0 as the recipient.
    assert.commandWorked(st.s.adminCommand({moveCollection: ns0, toShard: st.shard1.shardName}));
    assert.commandWorked(st.s.adminCommand({moveCollection: ns1, toShard: st.shard1.shardName}));

    // Perform a find command against both collections after the moveCollection operations above so
    // that the writes in the transactions below do not need to refresh the sharding metadata which
    // can lead to ExceededTimeLimit errors on slow machines.
    assert.eq(testColl0.find().itcount(), 1);
    assert.eq(testColl1.find().itcount(), 1);

    let beforeBlockingFp =
        configureFailPoint(configPrimary, "reshardingPauseCoordinatorBeforeBlockingWrites");
    let insertFp = configureFailPoint(
        shard1Primary, "hangDuringBatchInsert", {nss: ns0, shouldContinueOnInterrupt: true});
    let updateFp = configureFailPoint(
        shard1Primary, "hangDuringBatchUpdate", {nss: ns1, shouldContinueOnInterrupt: true});

    let moveCollThread = new Thread(runMoveCollection, st.s.host, ns0, st.shard0.shardName);
    moveCollThread.start();

    jsTest.log("Waiting for moveCollection to be about to enter the critical section");
    beforeBlockingFp.wait();

    jsTest.log("Starting a transaction on the donor shard involving the collection being moved");
    const lsid0 = {id: UUID()};
    const txnNumber0 = 15;
    let txnThread0 = new Thread(runInsertCmdInTxn,
                                st.s.host,
                                dbName,
                                collName0,
                                extractUUIDFromObject(lsid0.id),
                                txnNumber0,
                                [{x: 1}]);
    txnThread0.start();
    insertFp.wait();

    jsTest.log("Starting a transaction on the donor shard involving the other collection");
    const lsid1 = {id: UUID()};
    const txnNumber1 = 25;
    let txnThread1 = new Thread(runUpdateCmdInTxn,
                                st.s.host,
                                dbName,
                                collName1,
                                extractUUIDFromObject(lsid1.id),
                                txnNumber1,
                                [{q: {x: 0}, u: {$set: {x: 1}}, multi: false}]);
    txnThread1.start();
    updateFp.wait();

    jsTest.log("Unpausing moveCollection");
    beforeBlockingFp.off();

    // Upon transitioning to the "preparing-to-block-writes" state, shard1 should abort both
    // transactions, and the moveCollection operation should run to completion successfully.
    const moveCollRes = moveCollThread.returnData();
    assert.commandWorked(moveCollRes);

    jsTest.log("Verifying that both transactions got aborted");
    const txnRes0 = txnThread0.returnData();
    const txnRes1 = txnThread1.returnData();
    assertInterruptedWithTransientTransactionErrorLabel(txnRes0);
    assertInterruptedWithTransientTransactionErrorLabel(txnRes1);

    assert.commandFailedWithCode(shard1Primary.adminCommand(makeCommitTxnCmdObj(lsid0, txnNumber0)),
                                 ErrorCodes.NoSuchTransaction);
    assert.commandFailedWithCode(shard1Primary.adminCommand(makeCommitTxnCmdObj(lsid1, txnNumber1)),
                                 ErrorCodes.NoSuchTransaction);

    assert.eq(testColl0.find({x: 1}).itcount(), 0);
    assert.eq(testColl1.find({x: 1}).itcount(), 0);

    insertFp.off();
    updateFp.off();
}

/**
 * Tests transitioning to "preparing-to-block-writes" state while there are unprepared transactions
 * that have checked out the session to run commitTransaction and abortTransaction commands. Sets
 * 'reshardingAbortUnpreparedTransactionsUponPreparingToBlockWrites' to true. Verifies that the
 * donor aborts the transaction with InterruptedDueToReshardingCriticalSection error code and the
 * response has RetryableWriteError label.
 */
function testPreparingToBlockWritesWhileSessionCheckedOutCommitOrAbort() {
    jsTest.log("Testing preparing to block writes while the transaction has checked out the " +
               "session to run commitTransaction and abortTransaction command");
    testNum++;
    const collName0 = "testColl0_" + testNum;
    const collName1 = "testColl1_" + testNum;
    const ns0 = dbName + "." + collName0;
    const ns1 = dbName + "." + collName1;

    const testColl0 = testDB.getCollection(collName0);
    assert.commandWorked(testColl0.insert({x: 0}));

    const testColl1 = testDB.getCollection(collName1);
    assert.commandWorked(testColl1.insert({x: 0}));

    // By design, the primary shard is included as a recipient regardless of whether it is the shard
    // the collection is moving to. To make the moveCollection operation later in the test not have
    // a donor shard that also acts as a recipient, which would complicate the test, move
    // collection0 and also collection1 to shard1 (non-primary shard). That way, the moveCollection
    // operation will have shard1 as the donor and shard0 as the recipient.
    assert.commandWorked(st.s.adminCommand({moveCollection: ns0, toShard: st.shard1.shardName}));
    assert.commandWorked(st.s.adminCommand({moveCollection: ns1, toShard: st.shard1.shardName}));

    // Perform a find command against both collections after the moveCollection operations above so
    // that the writes in the transactions below do not need to refresh the sharding metadata which
    // can lead to ExceededTimeLimit errors on slow machines.
    assert.eq(testColl0.find().itcount(), 1);
    assert.eq(testColl1.find().itcount(), 1);

    let beforeBlockingFp =
        configureFailPoint(configPrimary, "reshardingPauseCoordinatorBeforeBlockingWrites");

    let moveCollThread = new Thread(runMoveCollection, st.s.host, ns0, st.shard0.shardName);
    moveCollThread.start();

    jsTest.log("Waiting for moveCollection to be about to enter the critical section");
    beforeBlockingFp.wait();

    jsTest.log("Starting a transaction on the donor shard involving the collection being moved");
    const lsid0 = {id: UUID()};
    const txnNumber0 = 15;
    assert.commandWorked(
        testDB.runCommand(makeInsertCmdObj(collName0, lsid0, txnNumber0, [{x: 1}])));

    jsTest.log("Starting a transaction on the donor shard involving the other collection");
    const lsid1 = {id: UUID()};
    const txnNumber1 = 25;
    assert.commandWorked(
        testDB.runCommand(makeInsertCmdObj(collName1, lsid1, txnNumber1, [{x: 1}])));

    jsTest.log(
        "Starting to commit the transaction on the donor shard involving the collection being moved");
    jsTest.log(
        "Starting to abort the transaction on the donor shard involving the other collection");
    let commitTxnFp = configureFailPoint(
        shard1Primary, "hangBeforeCommitingTxn", {uuid: lsid0.id, shouldCheckForInterrupt: true});
    let abortTxnFp =
        configureFailPoint(shard1Primary, "hangBeforeAbortingTxn", {shouldCheckForInterrupt: true});

    let txnThread0 = new Thread(
        runCommitTxnCmd, shard1Primary.host, extractUUIDFromObject(lsid0.id), txnNumber0);
    txnThread0.start();
    let txnThread1 =
        new Thread(runAbortTxnCmd, shard1Primary.host, extractUUIDFromObject(lsid1.id), txnNumber1);
    txnThread1.start();

    jsTest.log("Waiting for commitTransaction to block");
    commitTxnFp.wait();
    jsTest.log("Waiting for abortTransaction to block");
    abortTxnFp.wait();

    jsTest.log("Unpausing moveCollection");
    beforeBlockingFp.off();

    // Upon transitioning to the "preparing-to-block-writes" state, shard1 should abort both
    // transactions, and the moveCollection operation should run to completion successfully.
    const moveCollRes = moveCollThread.returnData();
    assert.commandWorked(moveCollRes);

    jsTest.log("Verifying that both transactions got aborted");
    const txnRes0 = txnThread0.returnData();
    const txnRes1 = txnThread1.returnData();
    assertInterruptedWithRetryableWriteErrorLabel(txnRes0);
    assertInterruptedWithRetryableWriteErrorLabel(txnRes1);

    assert.commandFailedWithCode(shard1Primary.adminCommand(makeCommitTxnCmdObj(lsid0, txnNumber0)),
                                 ErrorCodes.NoSuchTransaction);
    assert.commandFailedWithCode(shard1Primary.adminCommand(makeAbortTxnCmdObj(lsid1, txnNumber1)),
                                 ErrorCodes.NoSuchTransaction);

    assert.eq(testColl0.find({x: 1}).itcount(), 0);
    assert.eq(testColl1.find({x: 1}).itcount(), 0);

    commitTxnFp.off();
    abortTxnFp.off();
}

// Test that upon transitioning to "preparing-to-block-writes" state:
// 1. Donors abort unprepared transactions if 'reshardingAbortUnpreparedTransactionsUponPreparing-
//   ToBlockWrites' is enabled.
// 2. Donors do not abort unprepared transactions if the server parameter is disabled.
// 3. Donors do not abort prepared transactions whether or not the server parameter is enabled.
testPreparingToBlockWritesWhileSessionCheckedIn({
    preparedTxns: false,
    enableAbortUnpreparedTxns: true,
    expectAbort: true,
});
testPreparingToBlockWritesWhileSessionCheckedIn({
    preparedTxns: false,
    enableAbortUnpreparedTxns: false,
    expectAbort: false,
});
testPreparingToBlockWritesWhileSessionCheckedIn({
    preparedTxns: true,
    enableAbortUnpreparedTxns: true,
    expectAbort: false,
});

// Only run the cases below against unprepared transactions and with
// 'reshardingAbortUnpreparedTransactionsUponPreparingToBlockWrites' is enabled since we already
// verified above that donors don't abort transactions in the other cases.

// Test that if the in-progress command is not commitTransaction or abortTransaction, the response
// has TransientTransactionError label.
testPreparingToBlockWritesWhileSessionCheckedOutNonCommitOrAbort();

// Test that if the in-progress command is commitTransaction or abortTransaction, the response has
// RetryableWriteError label.
testPreparingToBlockWritesWhileSessionCheckedOutCommitOrAbort();

st.stop();
