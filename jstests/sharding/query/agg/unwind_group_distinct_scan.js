/**
 * Tests that a $unwind followed by a $group on the unwound field returns correct results through
 * mongos. The $unwind+$group to DISTINCT_SCAN rewrite currently does not apply to sharded collections.
 * TODO(SERVER-133187): Implement support for sharded collections
 *
 * @tags: [
 *   requires_fcv_91,
 * ]
 */

import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {getAggPlanStages} from "jstests/libs/query/analyze_plan.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const pipeline = [{$unwind: {path: "$a", preserveNullAndEmptyArrays: true}}, {$group: {_id: "$a"}}];

describe("$unwind+$group to DISTINCT_SCAN optimization through mongos", function () {
    let st;
    let db;

    before(function () {
        st = new ShardingTest({shards: 2});
        db = st.s.getDB(jsTestName());
        assert.commandWorked(
            st.s.adminCommand({enableSharding: db.getName(), primaryShard: st.shard0.shardName}),
        );
    });

    after(function () {
        st.stop();
    });

    function setupShardedCollection({coll, shardKey, middle, docs, index}) {
        assert.commandWorked(
            st.s.adminCommand({shardCollection: coll.getFullName(), key: shardKey}),
        );
        assert.commandWorked(st.s.adminCommand({split: coll.getFullName(), middle}));
        assert.commandWorked(
            st.s.adminCommand({
                moveChunk: coll.getFullName(),
                find: middle,
                to: st.shard1.shardName,
            }),
        );
        assert.commandWorked(coll.insertMany(docs));
        if (index) {
            assert.commandWorked(coll.createIndex(index));
        }
    }

    function assertResultsAndPlan({coll, pipeline, expectedResults, expectDistinctScan}) {
        assertArrayEq({actual: coll.aggregate(pipeline).toArray(), expected: expectedResults});
        const explain = coll.explain().aggregate(pipeline);
        const distinctScans = getAggPlanStages(explain, "DISTINCT_SCAN");
        if (expectDistinctScan) {
            assert.gt(distinctScans.length, 0, explain);
        } else {
            assert.eq(distinctScans.length, 0, explain);
        }
    }

    it("does not rewrite on a sharded collection", function () {
        const coll = db.sharded;
        setupShardedCollection({
            coll,
            shardKey: {_id: 1},
            middle: {_id: 0},
            docs: [{_id: -2, a: [1, 2]}, {_id: -1, a: []}, {_id: 1, a: [2, 3]}, {_id: 2}],
            index: {a: 1},
        });
        assertResultsAndPlan({
            coll,
            pipeline,
            expectedResults: [{_id: null}, {_id: 1}, {_id: 2}, {_id: 3}],
            expectDistinctScan: false,
        });
    });

    it("does not rewrite when grouping on the shard key", function () {
        const coll = db.shardKey;
        setupShardedCollection({
            coll,
            shardKey: {a: 1},
            middle: {a: 2},
            docs: [{a: 1}, {a: 1}, {a: 2}, {a: 3}, {b: 9}],
        });

        // The shard key is not allowed to be multikey.
        assert.writeErrorWithCode(coll.insert({a: [1, 2]}), ErrorCodes.ShardKeyNotFound);

        assertResultsAndPlan({
            coll,
            pipeline,
            expectedResults: [{_id: null}, {_id: 1}, {_id: 2}, {_id: 3}],
            expectDistinctScan: false,
        });
    });

    it("does not rewrite when grouping on the shard key with an index that is multikey on another field", function () {
        const coll = db.shardKeyMultikey;
        setupShardedCollection({
            coll,
            shardKey: {a: 1},
            middle: {a: 2},
            docs: [{a: 1, b: [1, 2]}, {a: 2, b: [3]}, {a: 3, b: 4}, {a: 4}],
            index: {a: 1, b: 1},
        });
        assertResultsAndPlan({
            coll,
            pipeline,
            expectedResults: [{_id: 1}, {_id: 2}, {_id: 3}, {_id: 4}],
            expectDistinctScan: false,
        });
    });

    it("does not rewrite when unwinding and grouping on a multikey field", function () {
        const coll = db.multikeyField;
        setupShardedCollection({
            coll,
            shardKey: {a: 1},
            middle: {a: 2},
            docs: [{a: 1, b: [1, 2]}, {a: 2, b: [2, 3]}, {a: 3, b: 4}, {a: 4}],
            index: {b: 1, a: 1},
        });
        assertResultsAndPlan({
            coll,
            pipeline: [
                {$unwind: {path: "$b", preserveNullAndEmptyArrays: true}},
                {$group: {_id: "$b"}},
            ],
            expectedResults: [{_id: null}, {_id: 1}, {_id: 2}, {_id: 3}, {_id: 4}],
            expectDistinctScan: false,
        });
    });

    it("rewrites on an unsharded collection", function () {
        const coll = db.unsharded;
        assert.commandWorked(coll.insertMany([{a: [1, 2]}, {a: []}, {b: 1}]));
        assert.commandWorked(coll.createIndex({a: 1}));

        assertResultsAndPlan({
            coll,
            pipeline,
            expectedResults: [{_id: null}, {_id: 1}, {_id: 2}],
            expectDistinctScan: true,
        });
    });
});
