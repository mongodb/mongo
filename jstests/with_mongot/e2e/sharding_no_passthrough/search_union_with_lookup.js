/**
 * Tests $search inside $lookup and $unionWith sub-pipelines across all four sharded/unsharded
 * base/search collection topology combinations.
 *
 * @tags: [
 *   requires_sharding,
 *   assumes_unsharded_collection,
 * ]
 */

import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {getShardNames} from "jstests/libs/cluster_helpers/sharded_cluster_fixture_helpers.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {createSearchIndex, dropSearchIndex} from "jstests/libs/query_integration_search/search.js";

const testDb = db.getSiblingDB(jsTestName());
const indexName = jsTestName() + "_index";

const baseUnsharded = testDb[jsTestName() + "_base_unsharded"];
const baseSharded = testDb[jsTestName() + "_base_sharded"];
const searchUnsharded = testDb[jsTestName() + "_search_unsharded"];
const searchSharded = testDb[jsTestName() + "_search_sharded"];

const baseDocs = [
    {_id: 1, localField: "chocolate cake"},
    {_id: 2, localField: "oatmeal cookies"},
    {_id: 3, localField: "banana bread"},
];

const baseShardedDocs = [
    {_id: 11, localField: "chocolate cake"},
    {_id: 12, localField: "oatmeal cookies"},
    {_id: 13, localField: "banana bread"},
];

const searchDocs = [
    {_id: 1, title: "chocolate cake", category: "dessert"},
    {_id: 2, title: "vanilla cake", category: "dessert"},
    {_id: 3, title: "red velvet cake", category: "dessert"},
    {_id: 4, title: "carrot cake", category: "dessert"},
    {_id: 5, title: "chocolate cookies", category: "snack"},
    {_id: 6, title: "oatmeal cookies", category: "snack"},
    {_id: 7, title: "sugar cookies", category: "snack"},
    {_id: 8, title: "banana bread", category: "bread"},
];

const searchShardedDocs = [
    {_id: 11, title: "chocolate cake", category: "dessert"},
    {_id: 12, title: "vanilla cake", category: "dessert"},
    {_id: 13, title: "red velvet cake", category: "dessert"},
    {_id: 14, title: "carrot cake", category: "dessert"},
    {_id: 15, title: "chocolate cookies", category: "snack"},
    {_id: 16, title: "oatmeal cookies", category: "snack"},
    {_id: 17, title: "sugar cookies", category: "snack"},
    {_id: 18, title: "banana bread", category: "bread"},
];

const cakeQuery = {index: indexName, text: {query: "cake", path: "title"}, count: {type: "total"}};

const topologyCases = [
    {
        name: "unsharded base, unsharded search",
        base: baseUnsharded,
        search: searchUnsharded,
        expectedSearchIds: [1, 2, 3, 4],
    },
    {
        name: "unsharded base, sharded search",
        base: baseUnsharded,
        search: searchSharded,
        expectedSearchIds: [11, 12, 13, 14],
    },
    {
        name: "sharded base, unsharded search",
        base: baseSharded,
        search: searchUnsharded,
        expectedSearchIds: [1, 2, 3, 4],
    },
    {
        name: "sharded base, sharded search",
        base: baseSharded,
        search: searchSharded,
        expectedSearchIds: [11, 12, 13, 14],
    },
];

describe("$search in $lookup and $unionWith across topologies", function () {
    before(function () {
        const shardNames = getShardNames(testDb.getMongo());
        assert.gte(shardNames.length, 2, "Test requires at least 2 shards");

        assert.commandWorked(
            testDb.adminCommand({enableSharding: testDb.getName(), primaryShard: shardNames[0]}),
        );

        baseUnsharded.drop();
        baseSharded.drop();
        searchUnsharded.drop();
        searchSharded.drop();

        assert.commandWorked(baseUnsharded.insertMany(baseDocs));
        assert.commandWorked(baseSharded.insertMany(baseShardedDocs));
        assert.commandWorked(searchUnsharded.insertMany(searchDocs));
        assert.commandWorked(searchSharded.insertMany(searchShardedDocs));

        assert.commandWorked(baseSharded.createIndex({_id: 1}));
        assert.commandWorked(
            testDb.adminCommand({shardCollection: baseSharded.getFullName(), key: {_id: 1}}),
        );
        assert.commandWorked(
            testDb.adminCommand({split: baseSharded.getFullName(), middle: {_id: 12}}),
        );
        assert.commandWorked(
            testDb.adminCommand({
                moveChunk: baseSharded.getFullName(),
                find: {_id: 13},
                to: shardNames[1],
                _waitForDelete: true,
            }),
        );

        assert.commandWorked(searchSharded.createIndex({_id: 1}));
        assert.commandWorked(
            testDb.adminCommand({shardCollection: searchSharded.getFullName(), key: {_id: 1}}),
        );
        assert.commandWorked(
            testDb.adminCommand({split: searchSharded.getFullName(), middle: {_id: 15}}),
        );
        assert.commandWorked(
            testDb.adminCommand({
                moveChunk: searchSharded.getFullName(),
                find: {_id: 15},
                to: shardNames[1],
                _waitForDelete: true,
            }),
        );

        createSearchIndex(searchUnsharded, {
            name: indexName,
            definition: {mappings: {dynamic: true}},
        });
        createSearchIndex(searchSharded, {
            name: indexName,
            definition: {mappings: {dynamic: true}},
        });
    });

    after(function () {
        dropSearchIndex(searchUnsharded, {name: indexName});
        dropSearchIndex(searchSharded, {name: indexName});
        baseUnsharded.drop();
        baseSharded.drop();
        searchUnsharded.drop();
        searchSharded.drop();
    });

    for (const tc of topologyCases) {
        it(`$lookup: ${tc.name}`, function () {
            const results = tc.base
                .aggregate([
                    {
                        $lookup: {
                            from: tc.search.getName(),
                            pipeline: [{$search: cakeQuery}, {$project: {_id: 1, title: 1}}],
                            as: "matches",
                        },
                    },
                ])
                .toArray();

            assert.eq(results.length, 3, {results});
            for (const doc of results) {
                assert.eq(doc.matches.length, 4, {doc});
                const matchIds = doc.matches.map((m) => m._id).sort((a, b) => a - b);
                assert.eq(matchIds, tc.expectedSearchIds, {doc});
            }
        });

        it(`$unionWith: ${tc.name}`, function () {
            const results = tc.base
                .aggregate([
                    {$project: {_id: 1, localField: 1}},
                    {
                        $unionWith: {
                            coll: tc.search.getName(),
                            pipeline: [{$search: cakeQuery}, {$project: {_id: 1, title: 1}}],
                        },
                    },
                ])
                .toArray();

            assert.eq(results.length, 7, {results});

            const baseIds = results.filter((d) => d.hasOwnProperty("localField")).map((d) => d._id);
            const searchIds = results.filter((d) => d.hasOwnProperty("title")).map((d) => d._id);

            assert.eq(baseIds.length, 3, {baseIds});
            assert.eq(searchIds.length, 4, {searchIds});

            assertArrayEq({actual: searchIds, expected: tc.expectedSearchIds});
        });

        it(`$$SEARCH_META count: ${tc.name}`, function () {
            const results = tc.base
                .aggregate([
                    {$project: {_id: 1}},
                    {
                        $lookup: {
                            from: tc.search.getName(),
                            pipeline: [
                                {$search: cakeQuery},
                                {$project: {_id: 1, meta: "$$SEARCH_META"}},
                            ],
                            as: "matches",
                        },
                    },
                ])
                .toArray();

            assert.eq(results.length, 3, {results});
            for (const doc of results) {
                assert.gt(doc.matches.length, 0, {doc});
                assert.eq(Number(doc.matches[0].meta.count.total), 4, {doc});
            }
        });
    }
});
