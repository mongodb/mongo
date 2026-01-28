/**
 * Tests that aggregations with $search + $out work correctly for untracked collections in
 * sharded clusters. This test verifies the fix for a bug where duplicate shard IDs were incorrectly
 * added to targetedShards for untracked collections.
 *
 * Test scenario:
 * 1. Create search index on untracked collection
 * 2. Run aggregation with $search + $out and verify it works correctly
 *
 * @tags: [ requires_fcv_83, requires_sharding, assumes_unsharded_collection ]
 */

import {createSearchIndex, dropSearchIndex} from "jstests/libs/search.js";
import {getShardNames} from "jstests/libs/sharded_cluster_fixture_helpers.js";

const testDb = db.getSiblingDB(jsTestName());
const shardNames = getShardNames(testDb.getMongo());
assert.gte(shardNames.length, 2, "Test requires at least 2 shards");
const shard0Name = shardNames[0];
const shard1Name = shardNames[1];

const inputDbName = "input_db";
const outputDbName = "output_db";

// Shard both databases
assert.commandWorked(db.adminCommand({enableSharding: inputDbName, primaryShard: shard0Name}));
assert.commandWorked(db.adminCommand({enableSharding: outputDbName, primaryShard: shard1Name}));

const inputColl = db.getSiblingDB(inputDbName).input_coll;
const outputColl = db.getSiblingDB(outputDbName).output_coll;
inputColl.drop();
outputColl.drop();

const indexDefinition = {
    name: "default",
    definition: {
        "mappings": {
            "dynamic": true,
        },
    },
};

const docs = [
    {_id: 1, title: "action movie", plot: "action packed thriller"},
    {_id: 2, title: "comedy film", plot: "funny comedy"},
    {_id: 3, title: "drama movie", plot: "dramatic action scene"},
];

// Don't shard the collection so that it is untracked.
assert.commandWorked(inputColl.insert(docs));
assert.commandWorked(createSearchIndex(inputColl, indexDefinition));

// Flush router config to ensure catalog cache reflects that collection is untracked
assert.commandWorked(db.adminCommand({flushRouterConfig: 1}));
const configDb = db.getSiblingDB("config");
assert.eq(configDb.collections.findOne({_id: inputColl.getFullName()}), null, "Input collection should be untracked");

const testSearchResult = inputColl
    .aggregate([{$search: {index: "default", text: {query: "action", path: "plot"}}}])
    .toArray();

inputColl.aggregate([
    {$search: {index: "default", text: {query: "action", path: "plot"}}},
    {$out: {db: outputDbName, coll: "output_coll"}},
]);

// Verify $out worked: search for "action" in "plot" should match documents with _id 1 and 3
const outputResults = outputColl.find().toArray();
assert.eq(
    outputResults.length,
    testSearchResult.length,
    "Output collection should have same number of documents as search results",
);
assert.eq(outputResults.length, 2, "$out should have written at least one document");

// Verify the documents in output collection match search results
const outputIds = outputResults.map((doc) => doc._id).sort();
const searchIds = testSearchResult.map((doc) => doc._id).sort();
assert.eq(outputIds.length, searchIds.length, "Output collection should have same number of documents as search");
assert.eq(outputIds, searchIds, "Output collection documents should match search results");

dropSearchIndex(inputColl, {name: "default"});
