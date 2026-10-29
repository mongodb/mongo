/**
 * E2E tests for searchSequenceToken on sharded collections. Validates token
 * projection before and after $_internalSplitPipeline at different merge points.
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

const docs = [
    {_id: 1, title: "cake recipe", sequence: 1},
    {_id: 2, title: "cake decorating", sequence: 2},
    {_id: 3, title: "cake baking tips", sequence: 3},
    {_id: 4, title: "cake frosting guide", sequence: 4},
    {_id: 11, title: "cake flavors", sequence: 11},
    {_id: 12, title: "cake layers", sequence: 12},
    {_id: 13, title: "cake tools", sequence: 13},
    {_id: 14, title: "cake ingredients", sequence: 14},
];

const searchQuery = {
    index: indexName,
    text: {query: "cake", path: "title"},
};

let shardNames;

function assertValidSequenceTokens(results, tokenField = "myToken") {
    assert.gt(results.length, 0, results);

    const tokens = results.map((result) => {
        assert(result.hasOwnProperty(tokenField), "missing token field", {result, tokenField});
        assert.eq(typeof result[tokenField], "string", result);
        assert.gt(result[tokenField].length, 0, result);
        return result[tokenField];
    });

    assert.eq(new Set(tokens).size, tokens.length, results);
}

function assertResultsFromBothShards(results) {
    const ids = results.map((d) => d._id);
    const hasLowIds = ids.some((id) => id < 10);
    const hasHighIds = ids.some((id) => id >= 10);
    assert(hasLowIds, "expected results from shard with _id < 10", {ids});
    assert(hasHighIds, "expected results from shard with _id >= 10", {ids});
}

describe("sharded search sequence token", function () {
    before(function () {
        shardNames = getShardNames(testDb.getMongo());
        assert.gte(shardNames.length, 2, "Test requires at least 2 shards");

        assert.commandWorked(
            testDb.adminCommand({enableSharding: testDb.getName(), primaryShard: shardNames[0]}),
        );

        testColl.drop();
        assert.commandWorked(testColl.insertMany(docs));

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
            definition: {
                mappings: {
                    dynamic: false,
                    fields: {
                        title: {type: "string"},
                        sequence: {type: "number"},
                    },
                },
            },
        });
    });

    after(function () {
        dropSearchIndex(testColl, {name: indexName});
        testColl.drop();
    });

    it("projects token after $_internalSplitPipeline with anyShard merge", function () {
        const results = testColl
            .aggregate([
                {$search: searchQuery},
                {$_internalSplitPipeline: {mergeType: "anyShard"}},
                {$project: {myToken: {$meta: "searchSequenceToken"}}},
            ])
            .toArray();

        assert.eq(results.length, docs.length, results);
        assertResultsFromBothShards(results);
        assertValidSequenceTokens(results);
    });

    it("projects token before $_internalSplitPipeline with anyShard merge", function () {
        const results = testColl
            .aggregate([
                {$search: searchQuery},
                {$project: {myToken: {$meta: "searchSequenceToken"}}},
                {$_internalSplitPipeline: {mergeType: "anyShard"}},
                {$addFields: {newField: 1}},
            ])
            .toArray();

        assert.eq(results.length, docs.length, results);
        assertResultsFromBothShards(results);
        assertValidSequenceTokens(results);
        results.forEach((doc) => {
            assert.eq(doc.newField, 1, doc);
        });
    });

    it("projects token after $_internalSplitPipeline with specificShard merge", function () {
        const results = testColl
            .aggregate([
                {$search: searchQuery},
                {$_internalSplitPipeline: {mergeType: {specificShard: shardNames[0]}}},
                {$project: {myToken: {$meta: "searchSequenceToken"}}},
            ])
            .toArray();

        assert.eq(results.length, docs.length, results);
        assertResultsFromBothShards(results);
        assertValidSequenceTokens(results);
    });

    it("projects token before $_internalSplitPipeline with specificShard merge", function () {
        const results = testColl
            .aggregate([
                {$search: searchQuery},
                {$project: {myToken: {$meta: "searchSequenceToken"}}},
                {$_internalSplitPipeline: {mergeType: {specificShard: shardNames[0]}}},
                {$addFields: {newField: 1}},
            ])
            .toArray();

        assert.eq(results.length, docs.length, results);
        assertResultsFromBothShards(results);
        assertValidSequenceTokens(results);
        results.forEach((doc) => {
            assert.eq(doc.newField, 1, doc);
        });
    });
});
