/*
 * Basic tests for moveRange when called only with max bound.
 *
 * TODO (SERVER-74536) remove this file and move these test cases to move_range_basic.js after 7.0
 * branches out.
 *
 * @tags: [
 *    requires_fcv_70,
 * ]
 */
'use strict';

load('jstests/sharding/libs/find_chunks_util.js');
load('jstests/sharding/libs/chunk_bounds_util.js');

var st = new ShardingTest({mongos: 1, shards: 2, chunkSize: 1});
var kDbName = 'db';

var mongos = st.s0;
var shard0 = st.shard0.shardName;
var shard1 = st.shard1.shardName;

assert.commandWorked(mongos.adminCommand({enableSharding: kDbName, primaryShard: shard0}));

function getRandomShardKeyValue(ns, skPattern) {
    const isHashedShardKey = Object.values(skPattern).includes('hashed');
    const coll = mongos.getCollection(ns);

    // Get a random document from the collection
    var doc = coll.aggregate([{$sample: {size: 1}}]).next();

    // Delete fields not making part of the shard key
    for (var key in doc) {
        if (!(key in skPattern)) {
            delete doc[key];
        }
    }

    if (isHashedShardKey) {
        doc.a = convertShardKeyToHashed(doc.a);
    }

    return doc;
}

function test(collName, skPattern) {
    const ns = kDbName + '.' + collName;
    assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: skPattern}));

    var aChunk = findChunksUtil.findOneChunkByNs(mongos.getDB('config'), ns, {shard: shard0});
    assert(aChunk);

    // Test that `moveRange` only with max bound works (translates to `moveChunk` because chunk too
    // small to be split)
    assert.commandWorked(mongos.adminCommand({moveRange: ns, max: aChunk.max, toShard: shard1}));

    assert.eq(0, mongos.getDB('config').chunks.countDocuments({_id: aChunk._id, shard: shard0}));
    assert.eq(1, mongos.getDB('config').chunks.countDocuments({_id: aChunk._id, shard: shard1}));

    // Test that `moveRange` only with max bound works (split+move)
    {
        // Insert 10MB in order to create big chunk (chunkSize is set to 1MB)
        const bigString = "X".repeat(1024 * 1024 / 4);  // 1 MB
        const coll = mongos.getCollection(ns);
        let bulk = coll.initializeUnorderedBulkOp();
        for (var i = 0; i < 10; i++) {
            bulk.insert({a: i, b: i, str: bigString});
        }
        assert.commandWorked(bulk.execute());

        // Get a random existing shard key value, `moveRange` will be called on the owning chunk
        var randomSK = getRandomShardKeyValue(ns, skPattern);

        // Get bounds and shard of the chunk owning `randomSK`
        const chunksBefore = findChunksUtil.findChunksByNs(mongos.getDB('config'), ns).toArray();
        const shardChunkBounds = chunkBoundsUtil.findShardChunkBounds(chunksBefore);
        const {shard, bounds} =
            chunkBoundsUtil.findShardAndChunkBoundsForShardKey(st, shardChunkBounds, randomSK);

        const donor = shard.shardName;
        const recipient = donor == shard0 ? shard1 : shard0;

        // Count chunks belonging to donor and recipient shards BEFORE moveRange
        const nChunksOnDonorBefore = chunksBefore.filter(chunk => chunk.shard == donor).length;
        const nChunksOnRecipientBefore =
            chunksBefore.filter(chunk => chunk.shard == recipient).length;

        assert.commandWorked(
            mongos.adminCommand({moveRange: ns, max: randomSK, toShard: recipient}));

        // Count chunks belonging to donor and recipient shards AFTER moveRange
        const chunksAfter = findChunksUtil.findChunksByNs(mongos.getDB('config'), ns).toArray();
        const nChunksOnDonorAfter = chunksAfter.filter(chunk => chunk.shard == donor).length;
        const nChunksOnRecipientAfter =
            chunksAfter.filter(chunk => chunk.shard == recipient).length;

        assert.eq(
            nChunksOnRecipientAfter,
            nChunksOnRecipientBefore + 1,
            "The number of chunks on the recipient shard did not increase following a moveRange");
        assert(nChunksOnDonorAfter == nChunksOnDonorBefore ||
                   nChunksOnDonorAfter == nChunksOnDonorBefore + 1,
               "Unexpected number of chunks on the donor shard after triggering a split + move");
    }
}

test('nonHashedShardKey', {a: 1});

test('nonHashedCompundShardKey', {a: 1, b: 1});

test('hashedShardKey', {a: 'hashed'});

st.stop();
