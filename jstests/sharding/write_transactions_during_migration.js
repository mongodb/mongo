/**
 * Tests that session information are properly transferred to the destination shard while
 * new writes are being sent to the source shard.
 */

load('./jstests/libs/chunk_manipulation_util.js');

/**
 * Test outline:
 * 1. Pause migration.
 * 2. Perform writes and allow it to be capture via OpObserver
 * 3. Unpause migration.
 * 4. Retry writes and confirm that writes are not duplicated.
 */
(function() {
    "use strict";

    load("jstests/libs/retryable_writes_util.js");

    if (!RetryableWritesUtil.storageEngineSupportsRetryableWrites(jsTest.options().storageEngine)) {
        jsTestLog("Retryable writes are not supported, skipping test");
        return;
    }

    var staticMongod = MongoRunner.runMongod({});  // For startParallelOps.

    var st = new ShardingTest({shards: {rs0: {nodes: 1}, rs1: {nodes: 1}}});
    st.adminCommand({enableSharding: 'test'});
    st.ensurePrimaryShard('test', st.shard0.shardName);
    st.adminCommand({shardCollection: 'test.user', key: {x: 1}});

    pauseMoveChunkAtStep(st.shard0, moveChunkStepNames.reachedSteadyState);
    var joinMoveChunk =
        moveChunkParallel(staticMongod, st.s.host, {x: 0}, null, 'test.user', st.shard1.shardName);

    waitForMoveChunkStep(st.shard0, moveChunkStepNames.reachedSteadyState);

    var insertCmd = {
        insert: 'user',
        documents: [{x: 10}, {x: 30}],
        ordered: false,
        lsid: {id: UUID()},
        txnNumber: NumberLong(34),
    };

    var testDB = st.getDB('test');
    var insertResult = assert.commandWorked(testDB.runCommand(insertCmd));

    var findAndModCmd = {
        findAndModify: 'user',
        query: {x: 30},
        update: {$inc: {y: 1}},
        new: true,
        upsert: true,
        lsid: {id: UUID()},
        txnNumber: NumberLong(37),
    };

    var findAndModifyResult = assert.commandWorked(testDB.runCommand(findAndModCmd));

    unpauseMoveChunkAtStep(st.shard0, moveChunkStepNames.reachedSteadyState);
    joinMoveChunk();

    var insertRetryResult = assert.commandWorked(testDB.runCommand(insertCmd));

    assert.eq(insertResult.ok, insertRetryResult.ok);
    assert.eq(insertResult.n, insertRetryResult.n);
    assert.eq(insertResult.writeErrors, insertRetryResult.writeErrors);
    assert.eq(insertResult.writeConcernErrors, insertRetryResult.writeConcernErrors);

    assert.eq(1, testDB.user.find({x: 10}).itcount());
    assert.eq(1, testDB.user.find({x: 30}).itcount());

    var findAndModifyRetryResult = assert.commandWorked(testDB.runCommand(findAndModCmd));

    assert.eq(findAndModifyResult.ok, findAndModifyRetryResult.ok);
    assert.eq(findAndModifyResult.value, findAndModifyRetryResult.value);
    assert.eq(findAndModifyResult.lastErrorObject, findAndModifyRetryResult.lastErrorObject);

    assert.eq(1, testDB.user.findOne({x: 30}).y);

    st.stop();

    MongoRunner.stopMongod(staticMongod);
})();
