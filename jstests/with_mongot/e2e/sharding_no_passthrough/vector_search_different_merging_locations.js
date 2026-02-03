/**
 * Tests that $vectorSearch produces correct results regardless of where merging happens
 * (mongos, anyShard, specificShard, localOnly).
 *
 * E2E version of with_mongot/vector_search_mocked/sharded_different_merging_locations.js
 *
 * @tags: [
 *   requires_fcv_71,
 *   requires_sharding,
 *   assumes_unsharded_collection,
 * ]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {createSearchIndex, dropSearchIndex} from "jstests/libs/search.js";
import {getShardNames} from "jstests/libs/sharded_cluster_fixture_helpers.js";

const collName = jsTestName();
const testColl = db.getCollection(collName);
const vectorSearchIndex = "vector_search_merge_index";
const queryVector = [1.0, 2.0, 3.0];

const vectorSearchQuery = {
    queryVector: queryVector,
    path: "x",
    numCandidates: 10,
    limit: 10,
    index: vectorSearchIndex,
};

// Expected order of results by cosine similarity to query vector [1.0, 2.0, 3.0].
const expectedIdOrder = [1, 2, 3, 4, 11, 12, 13, 14];
let shardNames;

/**
 * Tests $vectorSearch with a specific merge location using $_internalSplitPipeline.
 */
function testMergeAtLocation(mergeType, pipelineLimit = null) {
    // Use $_internalSplitPipeline to force merging at the specified location.
    const pipeline = [
        {$vectorSearch: vectorSearchQuery},
        {$_internalSplitPipeline: {mergeType: mergeType}},
        {$project: {_id: 1, score: {$meta: "vectorSearchScore"}}},
    ];

    if (pipelineLimit !== null) {
        pipeline.push({$limit: pipelineLimit});
    }

    const results = testColl.aggregate(pipeline).toArray();
    const expectedCount = pipelineLimit !== null ? Math.min(pipelineLimit, 8) : 8;
    const expectedIds = expectedIdOrder.slice(0, expectedCount);

    // Verify results match expected order, count, and structure.
    assert.eq(results.length, expectedCount, `mergeType: ${tojson(mergeType)}`);
    assert.eq(
        results.map((doc) => doc._id),
        expectedIds,
        `mergeType: ${tojson(mergeType)}`,
    );

    // Verify exact match is first with score = 1.0, and scores are strictly descending.
    assert.eq(results[0].score, 1, `mergeType: ${tojson(mergeType)}`);
    for (let i = 1; i < results.length; i++) {
        assert.gt(results[i - 1].score, results[i].score, `mergeType: ${tojson(mergeType)}`);
    }
}

describe("$vectorSearch with different merging locations", function () {
    before(function () {
        shardNames = getShardNames(db.getMongo());
        assert.gte(shardNames.length, 2, "Test requires at least 2 shards");

        assert.commandWorked(db.adminCommand({enableSharding: db.getName(), primaryShard: shardNames[0]}));

        testColl.drop();

        // Vectors progressively deviate from query [1.0, 2.0, 3.0] for deterministic score ordering.
        assert.commandWorked(
            testColl.insertMany([
                {_id: 1, x: [1.0, 2.0, 3.0]}, // exact match, score = 1.0
                {_id: 2, x: [1.0, 2.0, 3.5]}, // cos ≈ 0.997
                {_id: 3, x: [1.0, 2.0, 4.0]}, // cos ≈ 0.991
                {_id: 4, x: [1.0, 2.0, 5.0]}, // cos ≈ 0.976
                {_id: 11, x: [1.0, 2.0, 6.0]}, // cos ≈ 0.960
                {_id: 12, x: [1.0, 2.0, 7.0]}, // cos ≈ 0.945
                {_id: 13, x: [1.0, 2.0, 8.0]}, // cos ≈ 0.933
                {_id: 14, x: [1.0, 2.0, 9.0]}, // cos ≈ 0.922
            ]),
        );

        // Shard the test collection, split it at {_id: 10}, and move the higher chunk to shard1.
        assert.commandWorked(db.adminCommand({shardCollection: testColl.getFullName(), key: {_id: 1}}));
        assert.commandWorked(db.adminCommand({split: testColl.getFullName(), middle: {_id: 10}}));
        assert.commandWorked(
            db.adminCommand({
                moveChunk: testColl.getFullName(),
                find: {_id: 11},
                to: shardNames[1],
                _waitForDelete: true,
            }),
        );

        createSearchIndex(testColl, {
            name: vectorSearchIndex,
            type: "vectorSearch",
            definition: {
                fields: [{type: "vector", path: "x", numDimensions: queryVector.length, similarity: "cosine"}],
            },
        });
    });

    after(function () {
        dropSearchIndex(testColl, {name: vectorSearchIndex});
        testColl.drop();
    });

    it("works with mongos merge", function () {
        testMergeAtLocation("mongos");
        testMergeAtLocation("mongos", 3);
        testMergeAtLocation("mongos", 5);
        testMergeAtLocation("mongos", 10);
    });

    it("works with anyShard merge", function () {
        testMergeAtLocation("anyShard");
        testMergeAtLocation("anyShard", 3);
        testMergeAtLocation("anyShard", 5);
        testMergeAtLocation("anyShard", 10);
    });

    it("works with specificShard merge", function () {
        const owningShardMerge = {specificShard: shardNames[0]};
        testMergeAtLocation(owningShardMerge);
        testMergeAtLocation(owningShardMerge, 3);
        testMergeAtLocation(owningShardMerge, 5);
        testMergeAtLocation(owningShardMerge, 10);
    });

    it("works with localOnly merge", function () {
        testMergeAtLocation("localOnly");
        testMergeAtLocation("localOnly", 3);
        testMergeAtLocation("localOnly", 5);
        testMergeAtLocation("localOnly", 10);
    });
});
