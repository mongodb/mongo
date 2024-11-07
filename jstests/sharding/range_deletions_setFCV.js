/**
 * Tests that the range deleter updates the number of orphans from a migration with every deleted
 * orphan batch while running FCV upgrade.
 *
 * @tags: [
 *  uses_parallel_shell,
 *  requires_fcv_60
 * ]
 */

(function() {
'use strict';

load("jstests/libs/fail_point_util.js");
load('jstests/libs/parallel_shell_helpers.js');
load("jstests/sharding/libs/chunk_bounds_util.js");
load("jstests/sharding/libs/find_chunks_util.js");

const rangeDeleterBatchSize = 128;

const st = new ShardingTest({
    shards: 2,
    other: {
        shardOptions: {setParameter: {rangeDeleterBatchSize: rangeDeleterBatchSize}},
    }
});

const dbName = 'db';
const numDocsInColl = 1000;
const db = st.getDB(dbName);
const configDB = st.getDB('config');
const primaryShard = st.shard0;
assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: primaryShard.shardName}));

function checkNumOrphansOnRangeDeletionTask(conn, ns, numOrphans) {
    const rangeDeletionDoc =
        conn.getDB("config").getCollection("rangeDeletions").findOne({nss: ns});
    assert.neq(null,
               rangeDeletionDoc,
               "did not find document for namespace " + ns +
                   ", contents of config.rangeDeletions on " + conn + ": " +
                   tojson(conn.getDB("config").getCollection("rangeDeletions").find().toArray()));
    assert.eq(numOrphans,
              rangeDeletionDoc.numOrphanDocs,
              "Incorrect count of orphaned documents in config.rangeDeletions on " + conn +
                  ": expected " + numOrphans +
                  " orphaned documents but found range deletion document " +
                  tojson(rangeDeletionDoc));
}

function moveChunkAndCheckRangeDeletionTasksUponFCVUpgrade(nss, donorShard, moveChunkCmd) {
    // Ensure that no outstanding range deletion task will actually be executed (so that their
    // recovery docs may be inspected).
    let beforeDeletionFailpoint = configureFailPoint(donorShard, "hangBeforeDoingDeletion");
    let afterDeletionFailpoint = configureFailPoint(donorShard, "hangAfterDoingDeletion");

    // Downgrade the cluster
    assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: lastContinuousFCV}));

    // Upgrade the cluster - pausing the process before the "drain outstanding migrations" step
    let pauseBeforeDrainingMigrations =
        configureFailPoint(donorShard, "hangBeforeDrainingMigrations");
    const joinFCVUpgrade = startParallelShell(
        funWithArgs(function(fcv) {
            assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: fcv}));
        }, latestFCV), st.s.port);
    pauseBeforeDrainingMigrations.wait();
    // Complete a migration and check the pending range deletion tasks in the donor
    assert.commandWorked(db.adminCommand(moveChunkCmd));

    pauseBeforeDrainingMigrations.off();
    joinFCVUpgrade();
    // Check the batches are deleted correctly
    const numBatches = numDocsInColl / rangeDeleterBatchSize;
    assert(numBatches > 0);
    for (let i = 0; i < numBatches; i++) {
        // Wait for failpoint and check num orphans
        beforeDeletionFailpoint.wait();
        checkNumOrphansOnRangeDeletionTask(
            donorShard, nss, numDocsInColl - rangeDeleterBatchSize * i);
        // Unset and reset failpoint without allowing any batches deleted in the meantime
        afterDeletionFailpoint = configureFailPoint(donorShard, "hangAfterDoingDeletion");
        beforeDeletionFailpoint.off();
        afterDeletionFailpoint.wait();
        beforeDeletionFailpoint = configureFailPoint(donorShard, "hangBeforeDoingDeletion");
        afterDeletionFailpoint.off();
    }
    beforeDeletionFailpoint.off();
}

function testAgainstCollectionWithRangeShardKey() {
    // Fill up a sharded collection
    const coll = db['collWithRangeShardKey'];
    const nss = coll.getFullName();
    assert.commandWorked(st.s.adminCommand({shardCollection: nss, key: {_id: 1}}));

    // Insert some docs into the collection.
    let bulk = coll.initializeUnorderedBulkOp();
    for (let i = 0; i < numDocsInColl; i++) {
        bulk.insert({_id: i});
    }
    assert.commandWorked(bulk.execute());

    const moveChunkCmd = {moveChunk: nss, find: {_id: 0}, to: st.shard1.shardName};

    moveChunkAndCheckRangeDeletionTasksUponFCVUpgrade(nss, primaryShard, moveChunkCmd);
}

function testAgainstCollectionWithHashedShardKey() {
    const collName = 'collWithHashedShardKey';
    const hotKeyValue = 'hotKeyValue';
    const hashedKeyValue = convertShardKeyToHashed(hotKeyValue);
    const docWithHotShardKey = {k: hotKeyValue};
    const coll = db[collName];
    const nss = coll.getFullName();
    assert.commandWorked(
        st.s.adminCommand({shardCollection: nss, key: {k: 'hashed'}, numInitialChunks: 1}));

    // Insert some docs into the collection.
    let bulk = coll.initializeUnorderedBulkOp();
    for (let i = 0; i < numDocsInColl; i++) {
        bulk.insert(docWithHotShardKey);
    }
    assert.commandWorked(bulk.execute());

    // All the documents are supposed to be stored within a single shard
    const allCollChunks = findChunksUtil.findChunksByNs(configDB, nss).toArray();
    const chunksWithDoc = allCollChunks.filter((chunk) => {
        return chunkBoundsUtil.containsKey({k: hashedKeyValue}, chunk.min, chunk.max);
    });
    assert.eq(1, chunksWithDoc.length);
    const shardHoldingData = chunksWithDoc[0].shard === st.shard0.shardName ? st.shard0 : st.shard1;
    const shardWithoutData =
        shardHoldingData.shardName === st.shard0.shardName ? st.shard1 : st.shard0;

    const moveChunkCmd = {
        moveChunk: nss,
        bounds: [chunksWithDoc[0].min, chunksWithDoc[0].max],
        to: shardWithoutData.shardName
    };

    moveChunkAndCheckRangeDeletionTasksUponFCVUpgrade(nss, shardHoldingData, moveChunkCmd);
}

// Test Cases
testAgainstCollectionWithRangeShardKey();
testAgainstCollectionWithHashedShardKey();

st.stop();
})();
