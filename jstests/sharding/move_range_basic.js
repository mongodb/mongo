/*
 * Basic tests for moveRange.
 *
 * @tags: [
 *    assumes_balancer_off
 * ]
 */
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {chunkBoundsUtil} from "jstests/sharding/libs/chunk_bounds_util.js";
import {findChunksUtil} from "jstests/sharding/libs/find_chunks_util.js";

const st = new ShardingTest({mongos: 1, shards: 2, chunkSize: 1});
const kDbName = 'db';

const mongos = st.s0;
const shard0 = st.shard0.shardName;
const shard1 = st.shard1.shardName;

assert.commandWorked(mongos.adminCommand({enableSharding: kDbName, primaryShard: shard0}));

function getRandomShardKeyValue(ns, skPattern, filter) {
    const isHashedShardKey = Object.values(skPattern).includes('hashed');
    const coll = mongos.getCollection(ns);

    // Get a random document from the collection
    let doc = coll.aggregate([{$match: filter}, {$sample: {size: 1}}]).next();

    // Delete fields not making part of the shard key
    for (let key in doc) {
        if (!(key in skPattern)) {
            delete doc[key];
        }
    }

    if (isHashedShardKey) {
        doc.a = convertShardKeyToHashed(doc.a);
    }

    return doc;
}

// Tests for moveRange that will call move&split. We call moveRange with minBound on the positive
// values and moveRange with maxBound on negative values to ensure that the chunk chosen is big
// enough to be split by moveRange.
function testMoveRangeWithBigChunk(mongos, ns, skPattern, minBound) {
    // Get a random existing shard key value, `moveRange` will be called on the owning chunk
    let filter = minBound ? {a: {$gte: 0}} : {a: {$lt: 0}};
    let randomSK = getRandomShardKeyValue(ns, skPattern, filter);

    // Get bounds and shard of the chunk owning `randomSK`
    const chunksBefore = findChunksUtil.findChunksByNs(mongos.getDB('config'), ns).toArray();
    const shardChunkBounds = chunkBoundsUtil.findShardChunkBounds(chunksBefore);
    const {shard, bounds} =
        chunkBoundsUtil.findShardAndChunkBoundsForShardKey(st, shardChunkBounds, randomSK);

    const donor = shard.shardName;
    const recipient = donor == shard0 ? shard1 : shard0;

    // Count chunks belonging to donor and recipient shards BEFORE moveRange
    const nChunksOnDonorBefore = chunksBefore.filter(chunk => chunk.shard == donor).length;
    const nChunksOnRecipientBefore = chunksBefore.filter(chunk => chunk.shard == recipient).length;

    if (minBound) {
        assert.commandWorked(
            mongos.adminCommand({moveRange: ns, min: randomSK, toShard: recipient}));
    } else {
        assert.commandWorked(
            mongos.adminCommand({moveRange: ns, max: randomSK, toShard: recipient}));
    }

    // Count chunks belonging to donor and recipient shards AFTER moveRange
    const chunksAfter = findChunksUtil.findChunksByNs(mongos.getDB('config'), ns).toArray();
    const nChunksOnDonorAfter = chunksAfter.filter(chunk => chunk.shard == donor).length;
    const nChunksOnRecipientAfter = chunksAfter.filter(chunk => chunk.shard == recipient).length;

    let nExpectedChunksOnRecipientAfter = nChunksOnRecipientBefore + 1;
    // For moveRange with a maxBound, the number of chunks on recipient and donor doesn't change
    // if a shardKey that is a lower-bound of a pre-existing chunk was selected.
    if (!minBound && chunkBoundsUtil.eq(bounds[0], randomSK)) {
        nExpectedChunksOnRecipientAfter = nChunksOnRecipientBefore;
    }

    assert.eq(nExpectedChunksOnRecipientAfter,
              nChunksOnRecipientAfter,
              "The number of chunks on the recipient shard did not increase following a moveRange");
    assert(nChunksOnDonorAfter == nChunksOnDonorBefore ||
               nChunksOnDonorAfter == nChunksOnDonorBefore + 1,
           "Unexpected number of chunks on the donor shard after triggering a split + move");
}

function test(collName, skPattern) {
    const ns = kDbName + '.' + collName;

    assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: skPattern}));

    let aChunk = findChunksUtil.findOneChunkByNs(mongos.getDB('config'), ns, {shard: shard0});
    assert(aChunk);
    jsTest.log("Testing invalid commands");
    // Fail if one of the bounds is not a valid shard key
    assert.commandFailed(mongos.adminCommand(
        {moveRange: ns, min: aChunk.min, max: {invalidShardKey: 10}, toShard: shard1}));

    // Fail if the `to` shard does not exists
    assert.commandFailed(mongos.adminCommand(
        {moveRange: ns, min: aChunk.min, max: aChunk.max, toShard: 'WrongShard'}));

    // Test that `moveRange` with min & max bounds works
    jsTest.log("Testing moveRange with both bounds");
    assert.commandWorked(
        mongos.adminCommand({moveRange: ns, min: aChunk.min, max: aChunk.max, toShard: shard1}));

    assert.eq(0, mongos.getDB('config').chunks.countDocuments({_id: aChunk._id, shard: shard0}));
    assert.eq(1, mongos.getDB('config').chunks.countDocuments({_id: aChunk._id, shard: shard1}));

    // Test that `moveRange` only with min bound works (translates to `moveChunk` because chunk too
    // small to be split)
    jsTest.log("Testing moveRange with only min bound");
    assert.commandWorked(mongos.adminCommand({moveRange: ns, min: aChunk.min, toShard: shard0}));

    assert.eq(1, mongos.getDB('config').chunks.countDocuments({_id: aChunk._id, shard: shard0}));
    assert.eq(0, mongos.getDB('config').chunks.countDocuments({_id: aChunk._id, shard: shard1}));

    // Test that `moveRange` only with max bound works (translates to `moveChunk` because chunk too
    // small to be split)
    jsTest.log("Testing moveRange with only max bound");
    assert.commandWorked(mongos.adminCommand({moveRange: ns, max: aChunk.max, toShard: shard1}));

    assert.eq(0, mongos.getDB('config').chunks.countDocuments({_id: aChunk._id, shard: shard0}));
    assert.eq(1, mongos.getDB('config').chunks.countDocuments({_id: aChunk._id, shard: shard1}));

    // Insert 10MB >0 and <0 in order to create multiple big chunks (chunkSize is set to 1MB)
    jsTest.log("Inserting data to create large chunks");
    const bigString = "X".repeat(1024 * 1024 / 4);  // 1 MB
    const coll = mongos.getCollection(ns);
    let bulk = coll.initializeUnorderedBulkOp();
    for (let i = -10; i < 10; i++) {
        bulk.insert({a: i, b: i, str: bigString});
    }
    assert.commandWorked(bulk.execute());

    // Test moving large chunk with only min bound
    jsTest.log("Testing moveChunk with only min bound and large chunk");
    testMoveRangeWithBigChunk(mongos, ns, skPattern, true /* minBound */);

    // Test moving large chunk with only max bound
    jsTest.log("Testing moveChunk with only max bound and large chunk");
    testMoveRangeWithBigChunk(mongos, ns, skPattern, false /* maxBound */);
}

// Test running running moveRange on an unsplittable collection will fail
if (FeatureFlagUtil.isPresentAndEnabled(mongos, "TrackUnshardedCollectionsUponCreation")) {
    const collName = "unsplittable_collection"
    const ns = kDbName + '.' + collName;

    jsTest.log("Testing on unsplittable namespace");
    assert.commandWorked(
        mongos.getDB(kDbName).runCommand({createUnsplittableCollection: collName}));
    assert.commandFailedWithCode(
        mongos.adminCommand({moveRange: ns, min: {_id: 0}, toShard: shard0}),
        ErrorCodes.NamespaceNotSharded);
}

test('nonHashedShardKey', {a: 1});

test('nonHashedCompundShardKey', {a: 1, b: 1});

test('hashedShardKey', {a: 'hashed'});

st.stop();
