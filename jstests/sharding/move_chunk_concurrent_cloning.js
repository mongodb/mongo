/**
 * @tags: [
 *   featureFlagConcurrencyInChunkMigration,
 *   requires_fcv_63,
 * ]
 */
(function() {
"use strict";

load('./jstests/libs/chunk_manipulation_util.js');

const runParallelMoveChunk = (numThreads) => {
    // For startParallelOps to write its state
    let staticMongod = MongoRunner.runMongod({});

    let st = new ShardingTest({shards: 2});
    st.stopBalancer();

    const kThreadCount = numThreads;
    const kPadding = new Array(1024).join("x");

    let testDB = st.s.getDB('test');
    assert.commandWorked(testDB.adminCommand({enableSharding: 'test'}));
    st.ensurePrimaryShard('test', st.shard0.shardName);
    assert.commandWorked(testDB.adminCommand({shardCollection: 'test.user', key: {x: 1}}));

    let shardKeyVal = 0;
    const kDocsInBatch = 8 * 1000;
    const kMinCollSize = 128 * 1024 * 1024;
    let approxInsertedSize = 0;
    while (approxInsertedSize < kMinCollSize) {
        var bulk = testDB.user.initializeUnorderedBulkOp();
        for (let docs = 0; docs < kDocsInBatch; docs++) {
            shardKeyVal++;
            bulk.insert({_id: shardKeyVal, x: shardKeyVal, padding: kPadding});
        }
        assert.commandWorked(bulk.execute());

        approxInsertedSize = approxInsertedSize + (kDocsInBatch * 1024);
    }

    const kInitialLoadFinalKey = shardKeyVal;

    print(`Running tests with chunkMigrationConcurrency == ${kThreadCount}`);
    st._rs.forEach((replSet) => {
        assert.commandWorked(replSet.test.getPrimary().adminCommand(
            {setParameter: 1, chunkMigrationConcurrency: kThreadCount}));
    });

    const configCollEntry =
        st.s.getDB('config').getCollection('collections').findOne({_id: 'test.user'});
    let chunks = st.s.getDB('config').chunks.find({uuid: configCollEntry.uuid}).toArray();
    assert.eq(1, chunks.length, tojson(chunks));

    let joinMoveChunk =
        moveChunkParallel(staticMongod, st.s0.host, {x: 0}, null, 'test.user', st.shard1.shardName);

    // Migration cloning scans by shard key order. Perform some writes against the collection on
    // both the lower and upper ends of the shard key values while migration is happening to
    // exercise xferMods logic.
    const kDeleteIndexOffset = kInitialLoadFinalKey - 3000;
    const kUpdateIndexOffset = kInitialLoadFinalKey - 5000;
    for (let x = 0; x < 1000; x++) {
        assert.commandWorked(testDB.user.remove({x: x}));
        assert.commandWorked(testDB.user.update({x: 4000 + x}, {$set: {updated: true}}));

        assert.commandWorked(testDB.user.remove({x: kDeleteIndexOffset + x}));
        assert.commandWorked(
            testDB.user.update({x: kUpdateIndexOffset + x}, {$set: {updated: true}}));

        let newShardKey = kInitialLoadFinalKey + x + 1;
        assert.commandWorked(testDB.user.insert({_id: newShardKey, x: newShardKey}));
    }

    joinMoveChunk();

    let shardKeyIdx = 1000;  // Index starts at 1k since we deleted the first 1k docs.
    let cursor = testDB.user.find().sort({x: 1});

    while (cursor.hasNext()) {
        let next = cursor.next();
        assert.eq(next.x, shardKeyIdx);

        if ((shardKeyIdx >= 4000 && shardKeyIdx < 5000) ||
            (shardKeyIdx >= kUpdateIndexOffset && shardKeyIdx < (kUpdateIndexOffset + 1000))) {
            assert.eq(true, next.updated, tojson(next));
        }

        shardKeyIdx++;

        if (shardKeyIdx == kDeleteIndexOffset) {
            shardKeyIdx += 1000;
        }
    }

    shardKeyIdx--;
    assert.eq(shardKeyIdx, kInitialLoadFinalKey + 1000);

    // server Status on the receiving shard
    var serverStatus = st.shard1.getDB('admin').runCommand({serverStatus: 1});

    assert.eq(kThreadCount,
              serverStatus.shardingStatistics.chunkMigrationConcurrency,
              tojson(serverStatus));
    st.stop();
    MongoRunner.stopMongod(staticMongod);
};

// Run test 10 times with random concurrency levels.
for (let i = 1; i <= 5; i++) {
    runParallelMoveChunk(Math.floor(Math.random() * 31) + 1);
}
}

 )();
