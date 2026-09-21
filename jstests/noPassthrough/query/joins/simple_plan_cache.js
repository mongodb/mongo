/**
 * End to end test for the join plan cache. Verifies that an identical join query shape misses the
 * cache on first execution and hits it on the second, using the serverStatus counters.
 *
 * @tags: [
 *   requires_fcv_91,
 *   requires_sbe,
 * ]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {assertJoinPlanCacheStats} from "jstests/libs/query/join_utils.js";

describe("join plan cache", function () {
    before(function () {
        this.conn = MongoRunner.runMongod({
            setParameter: {
                internalEnableJoinOptimization: true,
                internalEnableJoinPlanCache: true,
            },
        });

        const db = this.conn.getDB(jsTestName());
        this.db = db;
        this.adminDB = this.conn.getDB("admin");
        assert.commandWorked(this.adminDB.adminCommand({clearJoinPlanCache: 1}));
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
            {
                $match: {a: {$gt: 0}},
            },
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

    after(function () {
        MongoRunner.stopMongod(this.conn);
    });

    it("misses the cache on the first run and hits it on the second run", function () {
        const emptyStats = this.adminDB.aggregate([{$joinPlanCacheStats: {}}]).toArray();
        assert.eq(0, emptyStats.length, "expected an empty join plan cache before the first run", {
            emptyStats,
        });

        // First run must miss and cache the plan.
        assertJoinPlanCacheStats({
            db: this.db,
            fn: () => {
                assert.eq(this.baseColl.aggregate(this.pipeline).toArray().length, 8);
            },
            expectedHits: 0,
            expectedMisses: 1,
        });

        const stats = this.adminDB.aggregate([{$joinPlanCacheStats: {}}]).toArray();
        assert.eq(
            1,
            stats.length,
            "expected exactly one join plan cache entry after the first run",
            {
                stats,
            },
        );

        // Second run is served from the cache.
        assertJoinPlanCacheStats({
            db: this.db,
            fn: () => {
                assert.eq(this.baseColl.aggregate(this.pipeline).toArray().length, 8);
            },
            expectedHits: 1,
            expectedMisses: 0,
        });
    });
});
