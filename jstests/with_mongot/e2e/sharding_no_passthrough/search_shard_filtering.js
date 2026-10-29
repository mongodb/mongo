/**
 * Tests that the _id lookups performed by $search have a shard filter applied to them so that
 * orphan documents are not returned.
 *
 * @tags: [
 *   requires_sharding,
 *   assumes_unsharded_collection,
 * ]
 */

import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {getShardNames} from "jstests/libs/cluster_helpers/sharded_cluster_fixture_helpers.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {createSearchIndex, dropSearchIndex} from "jstests/libs/query_integration_search/search.js";

TestData.skipCheckOrphans = true;

const testDb = db.getSiblingDB(jsTestName());
const collName = jsTestName();
const baseCollName = jsTestName() + "_base";

const testColl = testDb.getCollection(collName);
const baseColl = testDb.getCollection(baseCollName);

const indexName = jsTestName() + "_index";
// Matches every document that has an "x" field, which mongot on each shard will have indexed —
// including the orphan, so the shard filter on the $_internalSearchIdLookup is what must exclude
// it from the results.
const searchQuery = {index: indexName, exists: {path: "x"}};

let shardNames;
let shard0Conn;

describe("$search shard filtering", function () {
    before(function () {
        shardNames = getShardNames(testDb.getMongo());
        assert.gte(shardNames.length, 2, "Test requires at least 2 shards");

        assert.commandWorked(
            testDb.adminCommand({enableSharding: testDb.getName(), primaryShard: shardNames[0]}),
        );

        testColl.drop();

        assert.commandWorked(
            testColl.insertMany([
                {_id: 1, shardKey: 0, x: "ow"},
                {_id: 2, shardKey: 0, x: "now"},
                {_id: 3, shardKey: 0, x: "brown"},
                {_id: 4, shardKey: 0, x: "cow"},
                {_id: 11, shardKey: 100, x: "brown"},
                {_id: 12, shardKey: 100, x: "cow"},
                {_id: 13, shardKey: 100, x: "brown"},
                {_id: 14, shardKey: 100, x: "cow"},
            ]),
        );

        // Shard the test collection, split it at {shardKey: 10}, and move the higher chunk to
        // shard1.
        assert.commandWorked(testColl.createIndex({shardKey: 1}));
        assert.commandWorked(
            testDb.adminCommand({shardCollection: testColl.getFullName(), key: {shardKey: 1}}),
        );
        assert.commandWorked(
            testDb.adminCommand({split: testColl.getFullName(), middle: {shardKey: 10}}),
        );

        // 'waitForDelete' is set to 'true' so that range deletion completes before we insert our
        // orphan.
        assert.commandWorked(
            testDb.adminCommand({
                moveChunk: testColl.getFullName(),
                find: {shardKey: 100},
                to: shardNames[1],
                _waitForDelete: true,
            }),
        );

        const shardPrimaries = FixtureHelpers.getPrimaries(testDb);
        assert.gte(shardPrimaries.length, 2);

        for (const primary of shardPrimaries) {
            const count = primary.getDB(testDb.getName())[collName].find({shardKey: 0}).itcount();
            if (count > 0) {
                shard0Conn = primary;
                break;
            }
        }
        assert.neq(shard0Conn, null);

        // Insert an orphan document into shard 0 which is not owned by that shard.
        assert.commandWorked(
            shard0Conn
                .getDB(testDb.getName())
                [collName].insert({_id: 15, shardKey: 100, x: "should be filtered out"}),
        );

        // Insert a document into shard 0 which doesn't have a shard key. This document should not
        // be skipped when mongot returns a result indicating that it matched the text query.
        // The server should not crash and the operation should not fail.
        assert.commandWorked(
            shard0Conn.getDB(testDb.getName())[collName].insert({_id: 16, x: "no shard key"}),
        );

        createSearchIndex(testColl, {
            name: indexName,
            definition: {mappings: {dynamic: true}},
        });
    });

    after(function () {
        dropSearchIndex(testColl, {name: indexName});
        testColl.drop();
        baseColl.drop();
    });

    it("filters out orphan documents", function () {
        const results = testColl.aggregate([{$search: searchQuery}]).toArray();

        const resultIds = results.map((doc) => doc._id).sort((a, b) => a - b);

        assert(!resultIds.includes(15), "Orphan should be filtered out");
        // The document with _id 16 has no shard key. (Perhaps it was inserted manually). This
        // should not be filtered out, because documents with missing shard key values will be
        // placed on the chunk that they would be placed at if there were null values for the
        // shard key fields.
        assert(resultIds.includes(16), "Document without shard key should be included");

        const expectedIds = [1, 2, 3, 4, 11, 12, 13, 14, 16];
        assert.eq(resultIds, expectedIds);
    });

    it("filters orphans with getMore batching", function () {
        const results = testColl
            .aggregate([{$search: searchQuery}], {cursor: {batchSize: 2}})
            .toArray();

        const resultIds = results.map((doc) => doc._id).sort((a, b) => a - b);
        assert(!resultIds.includes(15));

        const expectedIds = [1, 2, 3, 4, 11, 12, 13, 14, 16];
        assert.eq(resultIds, expectedIds);
    });

    it("filters orphans in $unionWith subpipeline", function () {
        baseColl.drop();

        assert.commandWorked(
            baseColl.insertMany([
                {_id: 100, x: "base one"},
                {_id: 200, x: "base two"},
            ]),
        );

        // Shard the base collection so that shard filtering is exercised within the subpipeline.
        assert.commandWorked(baseColl.createIndex({_id: 1}));
        assert.commandWorked(
            testDb.adminCommand({shardCollection: baseColl.getFullName(), key: {_id: 1}}),
        );
        assert.commandWorked(
            testDb.adminCommand({split: baseColl.getFullName(), middle: {_id: 150}}),
        );
        assert.commandWorked(
            testDb.adminCommand({
                moveChunk: baseColl.getFullName(),
                find: {_id: 200},
                to: shardNames[1],
                _waitForDelete: true,
            }),
        );

        const results = baseColl
            .aggregate([
                {$unionWith: {coll: collName, pipeline: [{$search: searchQuery}]}},
                {$sort: {_id: 1}},
            ])
            .toArray();

        const resultIds = results.map((doc) => doc._id);

        // Expected result: base collection documents + search results (with orphan filtered out).
        assert(!resultIds.includes(15), "Orphan should be filtered in $unionWith");
        assert(resultIds.includes(16));

        const expectedIds = [1, 2, 3, 4, 11, 12, 13, 14, 16, 100, 200];
        assert.eq(
            resultIds.sort((a, b) => a - b),
            expectedIds,
        );

        // Verify orphan still exists on shard0 (wasn't accidentally deleted).
        assert.eq(shard0Conn.getDB(testDb.getName())[collName].find({_id: 15}).itcount(), 1);
    });
});
