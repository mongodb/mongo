/**
 * Tests that the _id lookups performed by $vectorSearch have a shard filter applied so that
 * orphan documents are not returned.
 *
 * E2E version of with_mongot/vector_search_mocked/vector_search_shard_filtering.js
 *
 * @tags: [
 *   requires_sharding,
 *   assumes_unsharded_collection,
 * ]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {createSearchIndex, dropSearchIndex} from "jstests/libs/search.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {getShardNames} from "jstests/libs/sharded_cluster_fixture_helpers.js";

TestData.skipCheckOrphans = true;

const collName = jsTestName();
const baseCollName = jsTestName() + "_base";

const testColl = db.getCollection(collName);
const baseColl = db.getCollection(baseCollName);

const vectorSearchIndex = "vector_search_shard_filtering_index";
const queryVector = [1.0, 2.0, 3.0];
const path = "x";
const numCandidates = 20;
const limit = 15;

const vectorSearchQuery = {queryVector, path, numCandidates, limit, index: vectorSearchIndex};

let shardNames;
let shard0Conn;

describe("$vectorSearch shard filtering", function () {
    before(function () {
        shardNames = getShardNames(db.getMongo());
        assert.gte(shardNames.length, 2, "Test requires at least 2 shards");

        assert.commandWorked(db.adminCommand({enableSharding: db.getName(), primaryShard: shardNames[0]}));

        testColl.drop();

        assert.commandWorked(
            testColl.insertMany([
                {_id: 1, shardKey: 0, x: [1.0, 2.0, 3.0]},
                {_id: 2, shardKey: 0, x: [1.1, 2.1, 3.1]},
                {_id: 3, shardKey: 0, x: [0.9, 1.9, 2.9]},
                {_id: 4, shardKey: 0, x: [0.8, 1.8, 2.8]},
                {_id: 11, shardKey: 100, x: [0.7, 1.7, 2.7]},
                {_id: 12, shardKey: 100, x: [0.6, 1.6, 2.6]},
                {_id: 13, shardKey: 100, x: [0.5, 1.5, 2.5]},
                {_id: 14, shardKey: 100, x: [0.4, 1.4, 2.4]},
            ]),
        );

        // Shard the test collection, split it at {shardKey: 10}, and move the higher chunk to shard1.
        assert.commandWorked(testColl.createIndex({shardKey: 1}));
        assert.commandWorked(db.adminCommand({shardCollection: testColl.getFullName(), key: {shardKey: 1}}));
        assert.commandWorked(db.adminCommand({split: testColl.getFullName(), middle: {shardKey: 10}}));

        // 'waitForDelete' is set to 'true' so that range deletion completes before we insert our orphan.
        assert.commandWorked(
            db.adminCommand({
                moveChunk: testColl.getFullName(),
                find: {shardKey: 100},
                to: shardNames[1],
                _waitForDelete: true,
            }),
        );

        const shardPrimaries = FixtureHelpers.getPrimaries(db);
        assert.gte(shardPrimaries.length, 2);

        for (const primary of shardPrimaries) {
            const count = primary.getDB(db.getName())[collName].find({shardKey: 0}).itcount();
            if (count > 0) {
                shard0Conn = primary;
                break;
            }
        }
        assert.neq(shard0Conn, null);

        // Insert an orphan document into shard 0 which is not owned by that shard.
        assert.commandWorked(
            shard0Conn.getDB(db.getName())[collName].insert({_id: 15, shardKey: 100, x: [0.99, 1.99, 2.99]}),
        );

        // Insert a document into shard 0 which doesn't have a shard key. This document should not be
        // skipped when mongot returns a result indicating that it matched the text query. The server
        // should not crash and the operation should not fail.
        assert.commandWorked(shard0Conn.getDB(db.getName())[collName].insert({_id: 16, x: [0.95, 1.95, 2.95]}));

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
        baseColl.drop();
    });

    it("filters out orphan documents", function () {
        const results = testColl.aggregate([{$vectorSearch: vectorSearchQuery}]).toArray();

        const resultIds = results.map((doc) => doc._id).sort((a, b) => a - b);

        assert(!resultIds.includes(15), "Orphan should be filtered out");
        // The document with _id 16 has no shard key. (Perhaps it was inserted manually). This should
        // not be filtered out, because documents with missing shard key values will be placed on the
        // chunk that they would be placed at if there were null values for the shard key fields.
        assert(resultIds.includes(16), "Document without shard key should be included");

        const expectedIds = [1, 2, 3, 4, 11, 12, 13, 14, 16];
        assert.eq(resultIds, expectedIds);
    });

    it("filters orphans with getMore batching", function () {
        const results = testColl.aggregate([{$vectorSearch: vectorSearchQuery}], {cursor: {batchSize: 2}}).toArray();

        const resultIds = results.map((doc) => doc._id).sort((a, b) => a - b);
        assert(!resultIds.includes(15));

        const expectedIds = [1, 2, 3, 4, 11, 12, 13, 14, 16];
        assert.eq(resultIds, expectedIds);
    });

    it("filters orphans in $unionWith subpipeline", function () {
        baseColl.drop();

        assert.commandWorked(
            baseColl.insertMany([
                {_id: 100, x: [0.1, 0.2, 0.3]},
                {_id: 200, x: [0.2, 0.3, 0.4]},
            ]),
        );

        // Set up base coll to test shard filtering works within subpipelines. Shard base collection.
        assert.commandWorked(baseColl.createIndex({_id: 1}));
        assert.commandWorked(db.adminCommand({shardCollection: baseColl.getFullName(), key: {_id: 1}}));
        assert.commandWorked(db.adminCommand({split: baseColl.getFullName(), middle: {_id: 150}}));
        assert.commandWorked(
            db.adminCommand({
                moveChunk: baseColl.getFullName(),
                find: {_id: 200},
                to: shardNames[1],
                _waitForDelete: true,
            }),
        );

        const results = baseColl
            .aggregate([
                {$unionWith: {coll: collName, pipeline: [{$vectorSearch: vectorSearchQuery}]}},
                {$sort: {_id: 1}},
            ])
            .toArray();

        const resultIds = results.map((doc) => doc._id);

        // Expected result: base collection documents + search results (with orphan filtered out).
        assert(!resultIds.includes(15), "Orphan should be filtered in $unionWith");
        assert(resultIds.includes(16));
        assert(resultIds.includes(100));
        assert(resultIds.includes(200));

        const expectedIds = [1, 2, 3, 4, 11, 12, 13, 14, 16, 100, 200];
        assert.eq(
            resultIds.sort((a, b) => a - b),
            expectedIds,
        );

        // Verify orphan still exists on shard0 (wasn't accidentally deleted).
        assert.eq(shard0Conn.getDB(db.getName())[collName].find({_id: 15}).itcount(), 1);
    });
});
