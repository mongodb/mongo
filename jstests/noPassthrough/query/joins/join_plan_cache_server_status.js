/**
 * End to end test for the join plan cache serverStatus metrics.
 *
 * @tags: [
 *   requires_fcv_90,
 *   requires_sbe,
 * ]
 */

import {afterEach, beforeEach, describe, it} from "jstests/libs/mochalite.js";

describe("join plan cache serverStatus metrics", function () {
    // Returns {hits, misses} from serverStatus.metrics.query.planCache.join.
    function joinPlanCacheStats(db) {
        const planCache = db.serverStatus().metrics.query.planCache;
        assert(planCache.hasOwnProperty("join"), "missing metrics.query.planCache.join", {
            planCache,
        });
        const join = planCache.join;
        return {hits: join.hits, misses: join.misses};
    }

    // Helper function to assert that the join plan cache stats have changed by
    // the expected amounts during the execution of the given function.
    function assertJoinPlanCacheStats({db, fn, expectedHits, expectedMisses}) {
        const beforeStats = joinPlanCacheStats(db);
        fn();
        const afterStats = joinPlanCacheStats(db);
        assert.eq(
            afterStats.hits - beforeStats.hits,
            expectedHits,
            "unexpected join plan cache hits",
            {
                expectedHits: expectedHits,
                before: beforeStats,
                after: afterStats,
            },
        );
        assert.eq(
            afterStats.misses - beforeStats.misses,
            expectedMisses,
            "unexpected join plan cache misses",
            {
                expectedMisses: expectedMisses,
                before: beforeStats,
                after: afterStats,
            },
        );
    }

    beforeEach(function () {
        this.conn = MongoRunner.runMongod({
            setParameter: {
                internalEnableJoinOptimization: true,
                internalEnableJoinPlanCache: true,
            },
        });

        const db = this.conn.getDB(jsTestName());
        this.db = db;
        this.baseColl = db[jsTestName()];
        this.foreignColl = db[jsTestName() + "_a"];

        assert.commandWorked(
            this.baseColl.insertMany([
                {a: 1, b: 1, d: 1},
                {a: 1, b: 2, d: 2},
                {a: 2, b: 1, d: 1},
                {a: 2, b: 2, d: 2},
            ]),
        );
        // Add index for multikeyness info for path arrayness.
        assert.commandWorked(this.baseColl.createIndex({dummy: 1, a: 1, b: 1, d: 1}));

        assert.commandWorked(
            this.foreignColl.insertMany([
                {a: 1, c: "foo", d: 1},
                {a: 1, c: "bar", d: 2},
                {a: 2, c: "baz", d: 1},
                {a: 2, c: "qux", d: 2},
            ]),
        );
        // Add index for multikeyness info for path arrayness.
        assert.commandWorked(this.foreignColl.createIndex({dummy: 1, a: 1, c: 1, d: 1}));

        this.pipeline = [
            {$match: {a: {$gt: 0}}},
            {
                $lookup: {
                    from: this.foreignColl.getName(),
                    localField: "a",
                    foreignField: "a",
                    as: "foreignColl",
                },
            },
            {$unwind: "$foreignColl"},
        ];
    });

    afterEach(function () {
        MongoRunner.stopMongod(this.conn);
    });

    it("increments misses on the first run and hits on the second run", function () {
        assertJoinPlanCacheStats({
            db: this.db,
            fn: () => {
                assert.eq(this.baseColl.aggregate(this.pipeline).toArray().length, 8);
            },
            expectedHits: 0,
            expectedMisses: 1,
        });
        assertJoinPlanCacheStats({
            db: this.db,
            fn: () => {
                assert.eq(this.baseColl.aggregate(this.pipeline).toArray().length, 8);
            },
            expectedHits: 1,
            expectedMisses: 0,
        });
    });

    it("different query shape increments misses", function () {
        assertJoinPlanCacheStats({
            db: this.db,
            fn: () => {
                assert.eq(this.baseColl.aggregate(this.pipeline).toArray().length, 8);
            },
            expectedHits: 0,
            expectedMisses: 1,
        });
        assertJoinPlanCacheStats({
            db: this.db,
            fn: () => {
                const differentShapePipeline = [
                    // change match expression predicate field to 'b' instead of 'a'
                    {$match: {b: {$gt: 0}}},
                    {
                        $lookup: {
                            from: this.foreignColl.getName(),
                            localField: "a",
                            foreignField: "a",
                            as: "foreignColl",
                        },
                    },
                    {$unwind: "$foreignColl"},
                ];
                assert.eq(this.baseColl.aggregate(differentShapePipeline).toArray().length, 8);
            },
            expectedHits: 0,
            expectedMisses: 1,
        });
    });

    it("plan cache disabled doesn't increment hits or misses", function () {
        // Disable the join plan cache.
        assert.commandWorked(
            this.db.adminCommand({setParameter: 1, internalEnableJoinPlanCache: false}),
        );

        assertJoinPlanCacheStats({
            db: this.db,
            fn: () => {
                assert.eq(this.baseColl.aggregate(this.pipeline).toArray().length, 8);
            },
            expectedHits: 0,
            expectedMisses: 0,
        });
    });
});
