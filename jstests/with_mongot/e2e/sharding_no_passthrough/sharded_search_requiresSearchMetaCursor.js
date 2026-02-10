/**
 * Checks that the requiresSearchMetaCursor is set properly on search requests dispatched from
 * mongos to mongod by running search queries and checking the shards' system.profile collection.
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

const dbName = jsTestName();
const shardedCollName = jsTestName() + "_sharded";
const unshardedCollName = jsTestName() + "_unsharded";
const searchIndexName = "test_search_index";

const shardedColl = db.getCollection(shardedCollName);
const unshardedColl = db.getCollection(unshardedCollName);

let shardPrimaries;
let shard0DB;
let shard1DB;

describe("requiresSearchMetaCursor in sharded search queries", function () {
    before(function () {
        const shardNames = getShardNames(db.getMongo());
        assert.gte(shardNames.length, 2, "Test requires at least 2 shards");

        // Set primary shard so the unsharded collection lives on a specific shard.
        assert.commandWorked(db.adminCommand({enableSharding: db.getName(), primaryShard: shardNames[0]}));

        shardedColl.drop();
        unshardedColl.drop();

        // Create a sharded collection to be used for $search and a separate unsharded collection.
        assert.commandWorked(
            shardedColl.insert([
                {_id: 1, x: "ow"},
                {_id: 2, x: "now", y: "lorem"},
                {_id: 3, x: "brown", y: "ipsum"},
                {_id: 4, x: "cow", y: "lorem ipsum"},
                {_id: 11, x: "brown", y: "ipsum"},
                {_id: 12, x: "cow", y: "lorem ipsum"},
                {_id: 13, x: "brown", y: "ipsum"},
                {_id: 14, x: "cow", y: "lorem ipsum"},
            ]),
        );
        assert.commandWorked(unshardedColl.insert([{b: 1}, {b: 3}, {b: 5}]));

        // Shard the test collection, split it at {_id: 10}, and move the higher chunk to shard1.
        assert.commandWorked(db.adminCommand({shardCollection: shardedColl.getFullName(), key: {_id: 1}}));
        assert.commandWorked(db.adminCommand({split: shardedColl.getFullName(), middle: {_id: 10}}));
        assert.commandWorked(
            db.adminCommand({
                moveChunk: shardedColl.getFullName(),
                find: {_id: 11},
                to: shardNames[1],
                _waitForDelete: true,
            }),
        );

        // Create search index on the sharded collection.
        // Use dynamic mapping to match all documents for the empty query.
        createSearchIndex(shardedColl, {
            name: searchIndexName,
            definition: {
                mappings: {
                    dynamic: true,
                },
            },
        });

        // Get shard connections for system.profile checking.
        shardPrimaries = FixtureHelpers.getPrimaries(db);
        assert.gte(shardPrimaries.length, 2);
        shard0DB = shardPrimaries[0].getDB(dbName);
        shard1DB = shardPrimaries[1].getDB(dbName);
    });

    after(function () {
        dropSearchIndex(shardedColl, {name: searchIndexName});
        shardedColl.drop();
        unshardedColl.drop();
    });

    function resetShardProfilers() {
        for (let shardDB of [shard0DB, shard1DB]) {
            shardDB.setProfilingLevel(0);
            shardDB.system.profile.drop();
            shardDB.setProfilingLevel(2);
        }
    }

    // Runs the given pipeline, then confirms via system.profile that the
    // requiresSearchMetaCursor field is set properly when commands are dispatched to shards.
    function runRequiresSearchMetaCursorTest({
        pipeline,
        coll,
        expectedDocs,
        shouldRequireSearchMetaCursor,
        hasSearchMetaStage = false,
        queryComment,
    }) {
        resetShardProfilers();

        const comment = queryComment ? queryComment : "search_query";
        const results = coll.aggregate(pipeline, {comment}).toArray();

        if (expectedDocs) {
            assert.eq(results, expectedDocs, "Results should match expected documents");
        } else {
            assert.gt(results.length, 0, "Expected at least one result");
        }

        // Check system.profile on both shards to verify requiresSearchMetaCursor is set correctly.
        for (let shardDB of [shard0DB, shard1DB]) {
            const res = shardDB.system.profile
                .find({"command.comment": comment, "command.aggregate": shardedCollName})
                .toArray();
            // Some shards may not have the command if they don't own any chunks for the query.
            if (res.length > 0) {
                assert.eq(1, res.length, res);
                if (hasSearchMetaStage) {
                    assert.eq(
                        shouldRequireSearchMetaCursor,
                        res[0].command.pipeline[0].$searchMeta.requiresSearchMetaCursor,
                        res,
                    );
                } else {
                    assert.eq(
                        shouldRequireSearchMetaCursor,
                        res[0].command.pipeline[0].$search.requiresSearchMetaCursor,
                        res,
                    );
                }
            }
        }
    }

    // Use an exists query on _id to match all documents in the collection.
    const mongotQuery = {
        index: searchIndexName,
        exists: {
            path: "_id",
        },
    };

    it("should not require search meta cursor for basic $search query", function () {
        runRequiresSearchMetaCursorTest({
            pipeline: [{$search: mongotQuery}],
            coll: shardedColl,
            shouldRequireSearchMetaCursor: false,
        });
    });

    it("should require search meta cursor when $$SEARCH_META is used in $project", function () {
        runRequiresSearchMetaCursorTest({
            pipeline: [{$search: mongotQuery}, {$limit: 1}, {$project: {meta: "$$SEARCH_META"}}],
            coll: shardedColl,
            shouldRequireSearchMetaCursor: true,
        });
    });

    it("should not require search meta cursor for $search with $sort and $limit only", function () {
        runRequiresSearchMetaCursorTest({
            pipeline: [{$search: mongotQuery}, {$sort: {_id: -1}}, {$limit: 2}],
            coll: shardedColl,
            expectedDocs: [
                {_id: 14, x: "cow", y: "lorem ipsum"},
                {_id: 13, x: "brown", y: "ipsum"},
            ],
            shouldRequireSearchMetaCursor: false,
        });
    });

    it("should require search meta cursor when $$SEARCH_META is used in $project after $sort", function () {
        runRequiresSearchMetaCursorTest({
            pipeline: [{$search: mongotQuery}, {$sort: {_id: -1}}, {$project: {_id: 0, foo: "$$SEARCH_META"}}],
            coll: shardedColl,
            expectedDocs: [
                {foo: {count: {lowerBound: NumberLong(8)}}},
                {foo: {count: {lowerBound: NumberLong(8)}}},
                {foo: {count: {lowerBound: NumberLong(8)}}},
                {foo: {count: {lowerBound: NumberLong(8)}}},
                {foo: {count: {lowerBound: NumberLong(8)}}},
                {foo: {count: {lowerBound: NumberLong(8)}}},
                {foo: {count: {lowerBound: NumberLong(8)}}},
                {foo: {count: {lowerBound: NumberLong(8)}}},
            ],
            shouldRequireSearchMetaCursor: true,
        });
    });

    it("should not require search meta cursor for $search with $sort, $limit, and $project without $$SEARCH_META", function () {
        runRequiresSearchMetaCursorTest({
            pipeline: [{$search: mongotQuery}, {$sort: {_id: -1}}, {$limit: 4}, {$project: {_id: 1}}],
            coll: shardedColl,
            expectedDocs: [{_id: 14}, {_id: 13}, {_id: 12}, {_id: 11}],
            shouldRequireSearchMetaCursor: false,
        });
    });

    it("should require search meta cursor when $$SEARCH_META is used in $addFields", function () {
        runRequiresSearchMetaCursorTest({
            pipeline: [{$search: mongotQuery}, {$limit: 1}, {$addFields: {meta: "$$SEARCH_META.count.lowerBound"}}],
            coll: shardedColl,
            shouldRequireSearchMetaCursor: true,
        });
    });

    it("should not require search meta cursor for $search with $group that doesn't use $$SEARCH_META", function () {
        runRequiresSearchMetaCursorTest({
            pipeline: [
                {$search: mongotQuery},
                {$group: {_id: "$y", x: {$addToSet: "$x"}}},
                {$match: {_id: {$ne: null}}},
                {$sort: {_id: 1}},
            ],
            coll: shardedColl,
            expectedDocs: [
                {_id: "ipsum", x: ["brown"]},
                {_id: "lorem", x: ["now"]},
                {_id: "lorem ipsum", x: ["cow"]},
            ],
            shouldRequireSearchMetaCursor: false,
        });
    });

    it("should require search meta cursor when $$SEARCH_META is used in $group", function () {
        runRequiresSearchMetaCursorTest({
            pipeline: [
                {$search: mongotQuery},
                {$group: {_id: "$x", meta: {$first: "$$SEARCH_META"}}},
                {$sort: {_id: -1}},
                {$limit: 2},
            ],
            coll: shardedColl,
            expectedDocs: [
                {_id: "ow", meta: {count: {lowerBound: NumberLong(8)}}},
                {_id: "now", meta: {count: {lowerBound: NumberLong(8)}}},
            ],
            shouldRequireSearchMetaCursor: true,
        });
    });

    it("should require search meta cursor when $$SEARCH_META is used in $project after $group", function () {
        runRequiresSearchMetaCursorTest({
            pipeline: [
                {$search: mongotQuery},
                {$sort: {_id: 1}},
                {$group: {_id: "$x", y: {$first: "$y"}}},
                {$sort: {_id: 1}},
                {$limit: 1},
                {$project: {_id: 1, y: 1, meta: "$$SEARCH_META"}},
            ],
            coll: shardedColl,
            expectedDocs: [{_id: "brown", y: "ipsum", meta: {count: {lowerBound: NumberLong(8)}}}],
            shouldRequireSearchMetaCursor: true,
        });
    });

    it("should require search meta cursor for $searchMeta stage", function () {
        runRequiresSearchMetaCursorTest({
            pipeline: [{$searchMeta: mongotQuery}],
            coll: shardedColl,
            shouldRequireSearchMetaCursor: true,
            hasSearchMetaStage: true,
            queryComment: "$searchMeta query",
        });
    });

    it("should require search meta cursor for $searchMeta with $limit and $project", function () {
        runRequiresSearchMetaCursorTest({
            pipeline: [{$searchMeta: mongotQuery}, {$limit: 1}, {$project: {val: 0}}],
            coll: shardedColl,
            shouldRequireSearchMetaCursor: true,
            hasSearchMetaStage: true,
        });
    });

    it("should require search meta cursor when $$SEARCH_META is used in $lookup subpipeline", function () {
        // Before running the following pipeline, make sure shard0 has up-to-date routing information.
        unshardedColl.aggregate([{$lookup: {from: shardedCollName, pipeline: [], as: "out"}}]);

        runRequiresSearchMetaCursorTest({
            pipeline: [
                {$match: {b: 3}},
                {
                    $lookup: {
                        from: shardedCollName,
                        pipeline: [
                            {$search: mongotQuery},
                            {$match: {y: "ipsum"}},
                            {$project: {_id: 1, meta: "$$SEARCH_META"}},
                            {$limit: 1},
                        ],
                        as: "out",
                    },
                },
                {$project: {_id: 0, b: 1, out: 1}},
            ],
            coll: unshardedColl,
            shouldRequireSearchMetaCursor: true,
        });
    });

    it("should require search meta cursor when $$SEARCH_META is used in $unionWith subpipeline", function () {
        runRequiresSearchMetaCursorTest({
            pipeline: [
                {$match: {b: {$gt: 3}}},
                {$project: {_id: 0}},
                {
                    $unionWith: {
                        coll: shardedCollName,
                        pipeline: [
                            {$search: mongotQuery},
                            {$project: {_id: 0, x: 1, meta: "$$SEARCH_META"}},
                            {$limit: 1},
                        ],
                    },
                },
            ],
            coll: unshardedColl,
            shouldRequireSearchMetaCursor: true,
        });
    });

    it("should not require search meta cursor when $$SEARCH_META is not used in $unionWith subpipeline", function () {
        runRequiresSearchMetaCursorTest({
            pipeline: [
                {$match: {b: {$gt: 3}}},
                {$project: {_id: 0}},
                {
                    $unionWith: {
                        coll: shardedCollName,
                        pipeline: [{$search: mongotQuery}, {$project: {_id: 0, x: 1}}, {$limit: 3}],
                    },
                },
            ],
            coll: unshardedColl,
            shouldRequireSearchMetaCursor: false,
        });
    });

    it("should not require search meta cursor when $$SEARCH_META is not used in $lookup subpipeline", function () {
        runRequiresSearchMetaCursorTest({
            pipeline: [
                {$match: {b: {$gt: 3}}},
                {$project: {_id: 0}},
                {
                    $lookup: {
                        from: shardedCollName,
                        pipeline: [{$search: mongotQuery}, {$project: {_id: 0, x: 1}}, {$limit: 1}],
                        as: "out",
                    },
                },
            ],
            coll: unshardedColl,
            shouldRequireSearchMetaCursor: false,
        });
    });
});
