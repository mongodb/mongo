/*
 * Test that prepared retryable internal transactions with a findAndModify statement can commit and
 * abort after failover and restart.
 *
 * @tags: [requires_fcv_60, uses_transactions, requires_persistence]
 */
(function() {
'use strict';

// For the test case where we abort a prepared internal transaction for retryable findAndModify with
// a pre/post image, the image collection on the primary is expected to be inconsistent with the
// image collection on secondaries. The reason is that for prepared transactions, the pre/post image
// is written to the image collection at prepare time, and on the primary the write is done in a
// side storage engine transaction, whereas on secondaries the write is done in the prepared
// transaction's storage engine transaction. Therefore, when the prepared transaction is aborted,
// the write to image collection only gets rolled back on secondaries.
TestData.skipCheckDBHashes = true;

load("jstests/replsets/rslib.js");
load("jstests/sharding/libs/sharded_transactions_helpers.js");

function runTest(st, stepDownShard0PrimaryFunc, testOpts = {
    runFindAndModifyWithPreOrPostImage,
    abortTxnAfterFailover,
    enableFindAndModifyImageCollection
}) {
    jsTest.log("Testing with options " + tojson(testOpts));

    const sessionUUID = UUID();
    const parentLsid = {id: sessionUUID};
    const parentTxnNumber = NumberLong(35);
    const childLsid = {id: parentLsid.id, txnNumber: parentTxnNumber, txnUUID: UUID()};
    const childTxnNumber = NumberLong(0);
    const stmtId = NumberInt(1);

    const kDbName = "testDb";
    const kCollName = "testColl-" + sessionUUID;
    let testDB = st.rs0.getPrimary().getDB(kDbName);
    let testColl = testDB.getCollection(kCollName);

    assert.commandWorked(testDB.adminCommand({
        setParameter: 1,
        storeFindAndModifyImagesInSideCollection: testOpts.enableFindAndModifyImageCollection
    }));

    assert.commandWorked(testDB.createCollection(kCollName));
    if (testOpts.runFindAndModifyWithPreOrPostImage) {
        assert.commandWorked(testColl.insert({_id: 0, x: 0}));
    }

    const findAndModifyCmdObj = {
        findAndModify: kCollName,
        query: {_id: 0, x: 0},
        update: {$inc: {x: 1}},
        upsert: true,
        lsid: childLsid,
        txnNumber: childTxnNumber,
        startTransaction: true,
        autocommit: false,
        stmtId: stmtId,
    };
    const prepareCmdObj = makePrepareTransactionCmdObj(childLsid, childTxnNumber);
    const commitCmdObj = makeCommitTransactionCmdObj(childLsid, childTxnNumber);
    const abortCmdObj = makeAbortTransactionCmdObj(childLsid, childTxnNumber);

    const initialRes = assert.commandWorked(testDB.runCommand(findAndModifyCmdObj));
    const prepareTxnRes = assert.commandWorked(testDB.adminCommand(prepareCmdObj));
    commitCmdObj.commitTimestamp = prepareTxnRes.prepareTimestamp;

    stepDownShard0PrimaryFunc();

    testDB = st.rs0.getPrimary().getDB(kDbName);
    testColl = testDB.getCollection(kCollName);

    if (testOpts.abortTxnAfterFailover) {
        assert.commandWorked(testDB.adminCommand(abortCmdObj));
        assert.eq(testColl.find({_id: 0, x: 1}).itcount(), 0);
    } else {
        assert.commandWorked(testDB.adminCommand(commitCmdObj));
        assert.eq(testColl.find({_id: 0, x: 1}).itcount(), 1);

        // Test that the findAndModify is retryable after failover.
        const retryRes = assert.commandWorked(testDB.runCommand(findAndModifyCmdObj));
        assert.commandWorked(testDB.adminCommand(commitCmdObj));
        assert.eq(initialRes.lastErrorObject, retryRes.lastErrorObject, retryRes);
        assert.eq(initialRes.value, retryRes.value);
        assert.eq(testColl.find({_id: 0, x: 1}).itcount(), 1);
    }
}

{
    jsTest.log("Test when the old primary steps up");
    const st = new ShardingTest({shards: 1, rs: {nodes: 1}});
    const stepDownShard0PrimaryFunc = () => {
        const oldPrimary = st.rs0.getPrimary();
        assert.commandWorked(
            oldPrimary.adminCommand({replSetStepDown: ReplSetTest.kForeverSecs, force: true}));
        assert.commandWorked(oldPrimary.adminCommand({replSetFreeze: 0}));
    };

    // Test findAnModify without pre/post image.
    runTest(st, stepDownShard0PrimaryFunc, {
        runFindAndModifyWithPreOrPostImage: false,
        abortTxnAfterFailover: false,
        enableFindAndModifyImageCollection: true
    });
    runTest(st, stepDownShard0PrimaryFunc, {
        runFindAndModifyWithPreOrPostImage: false,
        abortTxnAfterFailover: true,
        enableFindAndModifyImageCollection: true
    });

    // Test findAnModify with pre/post image when the image collection is enabled.
    runTest(st, stepDownShard0PrimaryFunc, {
        runFindAndModifyWithPreOrPostImage: true,
        abortTxnAfterFailover: false,
        enableFindAndModifyImageCollection: true
    });
    runTest(st, stepDownShard0PrimaryFunc, {
        runFindAndModifyWithPreOrPostImage: true,
        abortTxnAfterFailover: true,
        enableFindAndModifyImageCollection: true
    });

    st.stop();
}

{
    jsTest.log("Test when an old secondary steps up");
    const st = new ShardingTest({shards: 1, rs: {nodes: 2}});
    const stepDownShard0PrimaryFunc = () => {
        assert.commandWorked(st.rs0.getSecondary().adminCommand({replSetFreeze: 0}));
        assert.commandWorked(st.rs0.getPrimary().adminCommand(
            {replSetStepDown: ReplSetTest.kForeverSecs, force: true}));
    };

    // Test findAnModify without pre/post image.
    runTest(st, stepDownShard0PrimaryFunc, {
        runFindAndModifyWithPreOrPostImage: false,
        abortTxnAfterFailover: false,
        enableFindAndModifyImageCollection: true
    });
    runTest(st, stepDownShard0PrimaryFunc, {
        runFindAndModifyWithPreOrPostImage: false,
        abortTxnAfterFailover: true,
        enableFindAndModifyImageCollection: true
    });

    // Test findAnModify with pre/post image when the image collection is enabled.
    runTest(st, stepDownShard0PrimaryFunc, {
        runFindAndModifyWithPreOrPostImage: true,
        abortTxnAfterFailover: false,
        enableFindAndModifyImageCollection: true
    });
    runTest(st, stepDownShard0PrimaryFunc, {
        runFindAndModifyWithPreOrPostImage: true,
        abortTxnAfterFailover: true,
        enableFindAndModifyImageCollection: true
    });

    st.stop();
}

{
    jsTest.log("Test when a participant shard restarts");
    const st = new ShardingTest({shards: 1, rs: {nodes: 2}});
    const restartShard0Func = () => {
        st.rs0.stopSet(null /* signal */, true /*forRestart */);
        st.rs0.startSet({restart: true});
        st.rs0.getPrimary();
        // Wait for replication to recover the lastCommittedOpTime since it is illegal to run
        // commitTransaction before the prepare oplog entry has been majority committed.
        st.rs0.awaitLastOpCommitted();
    };

    // Test findAnModify without pre/post image.
    runTest(st, restartShard0Func, {
        runFindAndModifyWithPreOrPostImage: false,
        abortTxnAfterFailover: false,
        enableFindAndModifyImageCollection: true
    });
    runTest(st, restartShard0Func, {
        runFindAndModifyWithPreOrPostImage: false,
        abortTxnAfterFailover: true,
        enableFindAndModifyImageCollection: true
    });

    // Test findAnModify with pre/post image when the image collection is enabled.
    runTest(st, restartShard0Func, {
        runFindAndModifyWithPreOrPostImage: true,
        abortTxnAfterFailover: false,
        enableFindAndModifyImageCollection: true
    });
    runTest(st, restartShard0Func, {
        runFindAndModifyWithPreOrPostImage: true,
        abortTxnAfterFailover: true,
        enableFindAndModifyImageCollection: true
    });

    st.stop();
}
})();
