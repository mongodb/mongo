/**
 * Tests that $search results and SEARCH_META metadata (produced via the `count` operator) are
 * correct regardless of where the pipeline merge happens (mongos, anyShard, specificShard,
 * localOnly).
 *
 * @tags: [
 *   requires_sharding,
 *   assumes_unsharded_collection,
 * ]
 */

import {getShardNames} from "jstests/libs/cluster_helpers/sharded_cluster_fixture_helpers.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {createSearchIndex, dropSearchIndex} from "jstests/libs/query_integration_search/search.js";

const testDb = db.getSiblingDB(jsTestName());
const collName = jsTestName();
const testColl = testDb.getCollection(collName);

const indexName = jsTestName() + "_index";
const numDocs = 8;
const searchQuery = {
    index: indexName,
    range: {path: "year", gte: 2000, lt: 2100},
    count: {type: "total"},
};

let shardNames;

function assertMetaTotal(doc, expectedTotal) {
    assert(doc.hasOwnProperty("meta"), "expected doc to have meta", {doc});
    assert.eq(Number(doc.meta.count.total), expectedTotal, doc);
}

/**
 * Runs $search + $$SEARCH_META with merging forced to the given location and verifies both the
 * result set and that every document observes the merged metadata.
 */
function testMergeAtLocation(mergeType) {
    const results = testColl
        .aggregate([
            {$search: searchQuery},
            {$_internalSplitPipeline: {mergeType}},
            {$project: {_id: 1, meta: "$$SEARCH_META"}},
        ])
        .toArray();

    assert.eq(results.length, numDocs, `mergeType: ${tojson(mergeType)}`);
    const resultIds = results.map((doc) => doc._id).sort((a, b) => a - b);
    assert.eq(resultIds, [1, 2, 3, 4, 11, 12, 13, 14], `mergeType: ${tojson(mergeType)}`);
    results.forEach((doc) => assertMetaTotal(doc, numDocs));
}

/**
 * Runs $searchMeta with merging forced to the given location and verifies the merged metadata
 * document.
 */
function testMergeAtLocationSearchMeta(mergeType) {
    const results = testColl
        .aggregate([{$searchMeta: searchQuery}, {$_internalSplitPipeline: {mergeType}}])
        .toArray();

    assert.eq(results.length, 1, `mergeType: ${tojson(mergeType)}`);
    assert.eq(results[0].count, {total: NumberLong(numDocs)}, `mergeType: ${tojson(mergeType)}`);
}

describe("$search with different merging locations", function () {
    before(function () {
        shardNames = getShardNames(testDb.getMongo());
        assert.gte(shardNames.length, 2, "Test requires at least 2 shards");

        assert.commandWorked(
            testDb.adminCommand({enableSharding: testDb.getName(), primaryShard: shardNames[0]}),
        );

        testColl.drop();

        assert.commandWorked(
            testColl.insertMany([
                {_id: 1, year: 2001},
                {_id: 2, year: 2002},
                {_id: 3, year: 2003},
                {_id: 4, year: 2004},
                {_id: 11, year: 2011},
                {_id: 12, year: 2012},
                {_id: 13, year: 2013},
                {_id: 14, year: 2014},
            ]),
        );

        // Shard the test collection, split it at {_id: 10}, and move the higher chunk to shard1.
        assert.commandWorked(
            testDb.adminCommand({shardCollection: testColl.getFullName(), key: {_id: 1}}),
        );
        assert.commandWorked(
            testDb.adminCommand({split: testColl.getFullName(), middle: {_id: 10}}),
        );
        assert.commandWorked(
            testDb.adminCommand({
                moveChunk: testColl.getFullName(),
                find: {_id: 11},
                to: shardNames[1],
                _waitForDelete: true,
            }),
        );

        createSearchIndex(testColl, {
            name: indexName,
            definition: {mappings: {dynamic: false, fields: {year: {type: "number"}}}},
        });
    });

    after(function () {
        dropSearchIndex(testColl, {name: indexName});
        testColl.drop();
    });

    it("merges $search results and metadata on mongos", function () {
        testMergeAtLocation("mongos");
    });

    it("merges $search results and metadata on anyShard", function () {
        testMergeAtLocation("anyShard");
    });

    it("merges $search results and metadata on a specific shard", function () {
        testMergeAtLocation({specificShard: shardNames[0]});
    });

    it("merges $search results and metadata with localOnly", function () {
        testMergeAtLocation("localOnly");
    });

    it("merges $searchMeta on mongos", function () {
        testMergeAtLocationSearchMeta("mongos");
    });

    it("merges $searchMeta on anyShard", function () {
        testMergeAtLocationSearchMeta("anyShard");
    });

    it("merges $searchMeta on a specific shard", function () {
        testMergeAtLocationSearchMeta({specificShard: shardNames[0]});
    });
});
