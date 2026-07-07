/**
 * End to end test for the join plan cache. Verifies via server logs that an identical join query
 * shape misses the cache on first execution and hits it on the second.
 *
 * TODO(SERVER-129272): Implement this test without relying on server logs once we have commands
 * to inspect the join plan cache.
 *
 * @tags: [
 *   requires_fcv_90,
 *   requires_sbe,
 * ]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";

const JOIN_PLAN_CACHE_HIT_LOG_ID = 11083906;
const JOIN_PLAN_CACHE_MISS_LOG_ID = 11083907;

// Counts occurrences of a structured log line with the given numeric log ID.
function countLogId(logFile, id) {
    return cat(logFile)
        .split("\n")
        .filter((line) => line.length > 0)
        .map((line) => {
            try {
                return JSON.parse(line);
            } catch (e) {
                return null;
            }
        })
        .filter((entry) => entry && entry.id === id).length;
}

describe("join plan cache", function () {
    before(function () {
        this.conn = MongoRunner.runMongod({
            useLogFiles: true,
            setParameter: {
                internalEnableJoinOptimization: true,
                internalEnableJoinPlanCache: true,
            },
        });
        this.logFile = this.conn.fullOptions.logFile;

        const db = this.conn.getDB(jsTestName());
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

        // Raise query log verbosity so the join plan cache hit/miss log lines are emitted.
        assert.commandWorked(this.conn.getDB("admin").setLogLevel(5, "query"));
    });

    after(function () {
        MongoRunner.stopMongod(this.conn);
    });

    it("misses the cache on the first run and hits it on the second run", function () {
        assert.eq(this.baseColl.aggregate(this.pipeline).toArray().length, 8);
        assert.eq(
            countLogId(this.logFile, JOIN_PLAN_CACHE_MISS_LOG_ID),
            1,
            "expected exactly one join plan cache miss on the first run",
        );
        assert.eq(
            countLogId(this.logFile, JOIN_PLAN_CACHE_HIT_LOG_ID),
            0,
            "did not expect a join plan cache hit on the first run",
        );

        assert.eq(this.baseColl.aggregate(this.pipeline).toArray().length, 8);
        assert.gte(
            countLogId(this.logFile, JOIN_PLAN_CACHE_HIT_LOG_ID),
            1,
            "expected a join plan cache hit on the second run",
        );
    });
});
