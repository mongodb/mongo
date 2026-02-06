/**
 * Tests for $vectorSearch explain that require manual shard configuration.
 * This test validates sharding-specific behavior that requires control over how data is
 * distributed across shards - specifically exact per-shard stage counts, $limit optimization,
 * and read preference behavior (primary vs secondary).
 *
 * E2E version of with_mongot/vector_search_mocked/sharded_vector_search_explain.js
 *
 * @tags: [
 *   requires_sharding,
 *   assumes_unsharded_collection,
 * ]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {getAggPlanStages} from "jstests/libs/query/analyze_plan.js";
import {createSearchIndex, dropSearchIndex} from "jstests/libs/search.js";
import {getShardNames} from "jstests/libs/sharded_cluster_fixture_helpers.js";

const collName = jsTestName();

const queryVector = [1.0, 2.0, 3.0];
const path = "x";
const numCandidates = 100;
const vectorSearchLimit = 10;
const index = "vector_search_sharded_explain_index";

describe("$vectorSearch sharded explain", function () {
    before(function () {
        const shardNames = getShardNames(db.getMongo());
        assert.gte(shardNames.length, 2, "Test requires at least 2 shards");
        const primaryShardName = shardNames[0];
        const otherShardName = shardNames[1];

        // Enable sharding on the database with a specific primary shard.
        assert.commandWorked(db.adminCommand({enableSharding: db.getName(), primaryShard: primaryShardName}));

        const coll = db.getCollection(collName);
        coll.drop();

        // Insert documents that will be split across shards.
        // Documents with _id < 10 go to shard0, documents with _id >= 10 go to shard1.
        assert.commandWorked(
            coll.insertMany([
                // Shard 0 documents (_id < 10)
                {_id: 1, x: [1.0, 2.0, 3.0]},
                {_id: 2, x: [1.1, 2.1, 3.1]},
                {_id: 3, x: [0.9, 1.9, 2.9]},
                {_id: 4, x: [0.8, 1.8, 2.8]},
                // Shard 1 documents (_id >= 10)
                {_id: 11, x: [0.7, 1.7, 2.7]},
                {_id: 12, x: [0.6, 1.6, 2.6]},
                {_id: 13, x: [0.5, 1.5, 2.5]},
                {_id: 14, x: [0.4, 1.4, 2.4]},
            ]),
        );

        // Shard the collection and split at _id: 10.
        assert.commandWorked(db.adminCommand({shardCollection: coll.getFullName(), key: {_id: 1}}));
        assert.commandWorked(db.adminCommand({split: coll.getFullName(), middle: {_id: 10}}));
        assert.commandWorked(db.adminCommand({moveChunk: coll.getFullName(), find: {_id: 11}, to: otherShardName}));

        createSearchIndex(coll, {
            name: index,
            type: "vectorSearch",
            definition: {
                fields: [
                    {
                        type: "vector",
                        path: path,
                        numDimensions: 3,
                        similarity: "cosine",
                    },
                ],
            },
        });
    });

    after(function () {
        const coll = db.getCollection(collName);
        dropSearchIndex(coll, {name: index});
        coll.drop();
    });

    it("should optimize $limit to exact values on each shard and in merger", function () {
        const coll = db.getCollection(collName);
        const userLimit = vectorSearchLimit - 1;
        const expectedLimitVal = Math.min(userLimit, vectorSearchLimit);
        const pipeline = [
            {$vectorSearch: {queryVector, path, numCandidates, limit: vectorSearchLimit, index}},
            {$limit: userLimit},
        ];

        for (const verbosity of ["queryPlanner", "executionStats", "allPlansExecution"]) {
            const result = coll.explain(verbosity).aggregate(pipeline);

            // Should have exactly 2 $limit stages (one per shard).
            const limitStages = getAggPlanStages(result, "$limit");
            assert.eq(limitStages.length, 2, `Expected exactly 2 $limit stages: ${tojson(result)}`);

            // Each shard's $limit should be optimized to the minimum value.
            for (const limitStage of limitStages) {
                assert.eq(
                    expectedLimitVal,
                    limitStage["$limit"],
                    `Expected per-shard $limit value ${expectedLimitVal}: ${tojson(result)}`,
                );
            }

            // The merger $limit should also be the minimum value.
            const mergerLimitStage = result.splitPipeline.mergerPart.find((stage) => stage.hasOwnProperty("$limit"));
            assert(mergerLimitStage, `Expected $limit in mergerPart: ${tojson(result.splitPipeline)}`);
            assert.eq(
                expectedLimitVal,
                mergerLimitStage["$limit"],
                `Expected merger $limit value ${expectedLimitVal}: ${tojson(result.splitPipeline)}`,
            );
        }
    });

    it("should work with primary and secondary read preferences", function () {
        const coll = db.getCollection(collName);
        const pipeline = [{$vectorSearch: {queryVector, path, numCandidates, limit: vectorSearchLimit, index}}];

        for (const readPref of ["primary", "secondary"]) {
            db.getMongo().setReadPref(readPref);
            try {
                for (const verbosity of ["queryPlanner", "executionStats", "allPlansExecution"]) {
                    const result = coll.explain(verbosity).aggregate(pipeline);

                    // Verify explain succeeds and has expected structure.
                    assert(result.ok, `Explain should succeed with ${readPref} read pref: ${tojson(result)}`);

                    // Should have exactly 2 $vectorSearch stages (one per shard).
                    const vectorSearchStages = getAggPlanStages(result, "$vectorSearch");
                    assert.eq(
                        vectorSearchStages.length,
                        2,
                        `Expected 2 $vectorSearch stages with ${readPref} read pref: ${tojson(result)}`,
                    );

                    // Should have exactly 2 $_internalSearchIdLookup stages (one per shard).
                    const idLookupStages = getAggPlanStages(result, "$_internalSearchIdLookup");
                    assert.eq(
                        idLookupStages.length,
                        2,
                        `Expected 2 $_internalSearchIdLookup stages with ${readPref} read pref: ${tojson(result)}`,
                    );
                }
            } finally {
                db.getMongo().setReadPref("primary");
            }
        }
    });
});
