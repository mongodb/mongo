/**
 * Tests that $concatArrays is computed correctly for sharded collections.
 * @tags: [featureFlagArrayAccumulators, requires_fcv_81]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

const setUnionAcc = "$setUnion";
const concatArraysAcc = "$concatArrays";

const st = new ShardingTest({shards: 2, mongos: 1});

const collName = "agg_concat_arrays";
const db = st.s0.getDB("test");
const coll = db[collName];
const collUnsharded = db[collName + "_unsharded"];

assert.commandWorked(db.dropDatabase());

// Enable sharding on the test DB and ensure its primary is shard0.
assert.commandWorked(
    db.adminCommand({enableSharding: db.getName(), primaryShard: st.shard0.shardName}));

// Range-shard the test collection on _id.
assert.commandWorked(db.adminCommand({shardCollection: coll.getFullName(), key: {_id: 1}}));

// Split the collection into 4 chunks: [MinKey, -100), [-100, 0), [0, 100), [100, MaxKey].
assert.commandWorked(db.adminCommand({split: coll.getFullName(), middle: {_id: -100}}));
assert.commandWorked(db.adminCommand({split: coll.getFullName(), middle: {_id: 0}}));
assert.commandWorked(db.adminCommand({split: coll.getFullName(), middle: {_id: 100}}));

// Move the [0, 100) and [100, MaxKey] chunks to shard1.
assert.commandWorked(
    db.adminCommand({moveChunk: coll.getFullName(), find: {_id: 50}, to: st.shard1.shardName}));
assert.commandWorked(
    db.adminCommand({moveChunk: coll.getFullName(), find: {_id: 150}, to: st.shard1.shardName}));

function runArrayAccumTest(testName, accumulator) {
    // Check the result of the aggregation on the unsharded collection vs. the sharded collection to
    // ensure $concatArrays and $setUnion produce the same results in both.
    let pipeline = [{$sort: {_id: 1}}, {$group: {_id: null, lotsOfNumbers: {[accumulator]: '$x'}}}];
    if (accumulator === setUnionAcc) {
        // Need to add a stage to sort the values in the result array since the $setUnion
        // accumulator does not guarantee anything about the order of values.
        pipeline.push(
            {$project: {_id: 1, numbers: {$sortArray: {input: '$lotsOfNumbers', sortBy: 1}}}});
    }
    let res = coll.aggregate(pipeline).toArray();
    let resUnsharded = collUnsharded.aggregate(pipeline).toArray();
    assert.eq(
        res,
        resUnsharded,
        testName +
            `Got different results from the unsharded and sharded collections. Result from unsharded collection: ${
                tojson(resUnsharded)}. Result from sharded collection: ${tojson(res)}`);

    // For $concatArrays, run the same test again but with the reverse sort order to ensure
    // sortedness is being respected.
    if (accumulator === concatArraysAcc) {
        pipeline[0] = {$sort: {_id: -1}};
        res = coll.aggregate(pipeline).toArray();
        resUnsharded = collUnsharded.aggregate(pipeline).toArray();
        assert.eq(
            res,
            resUnsharded,
            testName +
                `Got different results from the unsharded and sharded collections. Result from unsharded collection: ${
                    tojson(resUnsharded)}. Result from sharded collection: ${tojson(res)}`);
    }

    // Test with grouping to ensure that sharded clusters correctly compute the groups. The extra
    // $set and $sort ensure that we do not rely on order of documents encountered for the final
    // pipeline result.
    pipeline = [
        {$sort: {_id: 1}},
        {$group: {_id: {$mod: ['$_id', 5]}, lotsOfNumbers: {[accumulator]: '$x'}}},
        {$set: {_id: {$mod: ['$_id', 5]}}},
    ];
    if (accumulator === setUnionAcc) {
        // Need to add a stage to sort the values in the result array since the $setUnion
        // accumulator does not guarantee anything about the order of values.
        pipeline.push(
            {$project: {_id: 1, numbers: {$sortArray: {input: '$lotsOfNumbers', sortBy: 1}}}},
        );
    }
    pipeline.push({$sort: {_id: 1}});

    res = coll.aggregate(pipeline).toArray();
    resUnsharded = collUnsharded.aggregate(pipeline).toArray();
    assert.eq(
        res,
        resUnsharded,
        testName +
            `Got different results from the unsharded and sharded collections. Result from unsharded collection: ${
                tojson(resUnsharded)}. Result from sharded collection: ${tojson(res)}`);
}

(function testMergingAcrossShards() {
    coll.remove({});
    collUnsharded.remove({});

    // Write 400 documents across the 4 chunks. Values of 'val' field are distributed across both
    // shards such that they do not overlap so we can test sortedness properties.
    for (let i = -200; i < 200; i++) {
        // Create a document where 'x' is an array which is a prerequisite for $concatArrays.
        const docToInsert = {_id: i, x: [i]};
        assert.commandWorked(coll.insert(docToInsert));
        assert.commandWorked(collUnsharded.insert(docToInsert));
    }

    runArrayAccumTest("testMergingAcrossAllShards", concatArraysAcc);
    runArrayAccumTest("testMergingAcrossAllShards", setUnionAcc);
})();

(function testMergingOneShardHasArrays() {
    coll.remove({});
    collUnsharded.remove({});

    // Write 100 documents that all land in the same shard.
    for (let i = 110; i < 210; i++) {
        // Create a document where 'x' is an array which is a prerequisite for $concatArrays.
        const docToInsert = {_id: i, x: [i]};
        assert.commandWorked(coll.insert(docToInsert));
        assert.commandWorked(collUnsharded.insert(docToInsert));
    }

    runArrayAccumTest("testMergingOneShardHasArrays", concatArraysAcc);
    runArrayAccumTest("testMergingOneShardHasArrays", setUnionAcc);
})();

(function testMergingNoShardsHaveArrays() {
    coll.remove({});
    collUnsharded.remove({});

    for (let i = -200; i < 200; i++) {
        // Insert documents without the 'x' field that is expected to contain an array
        const docToInsert = {_id: i};
        assert.commandWorked(coll.insert(docToInsert));
        assert.commandWorked(collUnsharded.insert(docToInsert));
    }

    runArrayAccumTest("testMergingNoShardsHaveArrays", concatArraysAcc);
    runArrayAccumTest("testMergingNoShardsHaveArrays", setUnionAcc);
})();

(function testMergingNoShardsHaveDocuments() {
    coll.remove({});
    collUnsharded.remove({});

    runArrayAccumTest("testMergingNoShardsHaveDocuments", concatArraysAcc);
    runArrayAccumTest("testMergingNoShardsHaveDocuments", setUnionAcc);
})();

(function testWithOverlappingDatasets() {
    coll.remove({});
    collUnsharded.remove({});

    // Write 400 documents across the 4 chunks. Values of 'val' field are distributed across both
    // shards such that their ranges overlap but the datasets aren't identical
    for (let i = -200; i < 200; i++) {
        const val = (i + 200) % 270 + 1;  // [1, ..., 200] & [201, ..., 270, 1, ..., 130]
        const docToInsert = {_id: i, x: [val]};
        assert.commandWorked(coll.insert(docToInsert));
        assert.commandWorked(collUnsharded.insert(docToInsert));
    }

    runArrayAccumTest("testWithOverlappingDatasets", concatArraysAcc);
    runArrayAccumTest("testWithOverlappingDatasets", setUnionAcc);
})();

st.stop();
