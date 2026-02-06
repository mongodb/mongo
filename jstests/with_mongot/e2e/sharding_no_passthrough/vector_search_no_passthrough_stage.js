/**
 * Tests that $vectorSearch with vectorSearchScore succeeds on unsharded collections in sharded
 * clusters even with stages that can't be passed to shards (like $out).
 *
 * E2E version of with_mongot/vector_search_mocked/sharded_vector_search_no_passthrough_stage.js
 *
 * @tags: [
 *   requires_sharding,
 *   assumes_unsharded_collection,
 * ]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {createSearchIndex, dropSearchIndex} from "jstests/libs/search.js";
import {getShardNames} from "jstests/libs/sharded_cluster_fixture_helpers.js";

const collName = jsTestName();
const testColl = db.getCollection(collName);

const foreignCollName = jsTestName() + "_output";
const foreignColl = db.getCollection(foreignCollName);

const vectorSearchIndex = "vector_search_out_index";
const queryVector = [1.0, 2.0, 3.0];
const path = "x";
const numCandidates = 10;
const limit = 5;

describe("$vectorSearch with $out on unsharded collection", function () {
    before(function () {
        const shardNames = getShardNames(db.getMongo());
        assert.gte(shardNames.length, 2, "Test requires at least 2 shards");

        // Set primary shard so the unsharded collection lives on a specific shard.
        assert.commandWorked(db.adminCommand({enableSharding: db.getName(), primaryShard: shardNames[0]}));

        testColl.drop();
        foreignColl.drop();

        assert.commandWorked(
            testColl.insertMany([
                {_id: 1, x: [1.0, 2.0, 3.0]},
                {_id: 2, x: [1.1, 2.1, 3.1]},
                {_id: 3, x: [0.9, 1.9, 2.9]},
                {_id: 4, x: [0.8, 1.8, 2.8]},
            ]),
        );

        createSearchIndex(testColl, {
            name: vectorSearchIndex,
            type: "vectorSearch",
            definition: {
                fields: [{type: "vector", path: path, numDimensions: queryVector.length, similarity: "cosine"}],
            },
        });
    });

    after(function () {
        dropSearchIndex(testColl, {name: vectorSearchIndex});
        testColl.drop();
        foreignColl.drop();
    });

    it("succeeds with vectorSearchScore and $out stage", function () {
        testColl.aggregate(
            [
                {$vectorSearch: {queryVector, path, numCandidates, limit, index: vectorSearchIndex}},
                {$project: {_id: 1, x: 1, score: {$meta: "vectorSearchScore"}}},
                {$out: foreignColl.getName()},
            ],
            {cursor: {}},
        );

        const outputResults = foreignColl.find().sort({score: -1}).toArray();

        assert.eq(outputResults.length, 4);

        const expectedOrder = [1, 2, 3, 4];
        const actualOrder = outputResults.map((doc) => doc._id);
        assert.eq(actualOrder, expectedOrder);

        outputResults.forEach((doc) => {
            assert(doc.hasOwnProperty("_id"));
            assert(doc.hasOwnProperty("x"));
            assert(doc.hasOwnProperty("score"));
            assert.gt(doc.score, 0);
        });

        assert.eq(outputResults[0]._id, 1);
        assert.eq(outputResults[0].score, 1);

        for (let i = 1; i < outputResults.length; i++) {
            // Results should be sorted in descending order by score.
            assert.gt(outputResults[i - 1].score, outputResults[i].score);
        }
    });
});
