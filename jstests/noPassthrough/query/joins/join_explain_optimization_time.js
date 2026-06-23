/**
 * Tests that `optimizationTimeMillis` and `optimizationTimeMicros` are correctly measured and
 * reported in explain output when join optimization (JOO) is used.
 *
 * This test injects a delay via the `sleepWhileJoinOptimizing` failpoint and asserts the
 * reported time reflects the delay.
 *
 * @tags: [
 *   requires_fcv_90,
 *   requires_sbe,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {getQueryPlanner} from "jstests/libs/query/analyze_plan.js";
import {describe, it, before, after} from "jstests/libs/mochalite.js";

const kSleepMs = 500;

describe("optimizationTimeMillis with join optimization", function () {
    const conn = MongoRunner.runMongod({setParameter: {featureFlagPathArrayness: true}});
    assert.neq(conn, null, "mongod failed to start up");

    // Enable join optimization.
    assert.commandWorked(
        conn.adminCommand({setParameter: 1, internalEnableJoinOptimization: true}),
    );

    const db = conn.getDB(jsTestName());
    const coll = db[jsTestName()];
    const foreignColl = db[jsTestName() + "_2"];

    const pipeline = [
        {$lookup: {from: foreignColl.getName(), localField: "a", foreignField: "a", as: "joined"}},
        {$unwind: "$joined"},
    ];

    before(function () {
        coll.drop();
        foreignColl.drop();

        assert.commandWorked(
            coll.insertMany([
                {_id: 0, a: 1},
                {_id: 1, a: 2},
                {_id: 2, a: 3},
            ]),
        );
        assert.commandWorked(
            foreignColl.insertMany([
                {_id: 0, a: 1},
                {_id: 1, a: 2},
                {_id: 2, a: 3},
            ]),
        );

        // Create dummy indexes to provide PathArrayness information.
        assert.commandWorked(coll.createIndex({dummy: 1, a: 1}));
        assert.commandWorked(foreignColl.createIndex({dummy: 1, a: 1}));
    });

    after(function () {
        MongoRunner.stopMongod(conn);
    });

    it("reports optimizationTimeMillis and optimizationTimeMicros >= injected delay", function () {
        const fp = configureFailPoint(conn, "sleepWhileJoinOptimizing", {ms: kSleepMs});
        try {
            const explain = coll.explain().aggregate(pipeline);
            jsTest.log.info("JOO explain output", {explain});

            const queryPlanner = getQueryPlanner(explain);
            assert(
                queryPlanner.hasOwnProperty("optimizationTimeMillis"),
                "optimizationTimeMillis missing from queryPlanner",
                {queryPlanner},
            );
            assert.gte(
                queryPlanner.optimizationTimeMillis,
                kSleepMs,
                "optimizationTimeMillis should be >= injected sleep delay",
                {queryPlanner},
            );
            assert(
                queryPlanner.hasOwnProperty("optimizationTimeMicros"),
                "optimizationTimeMicros missing from queryPlanner",
                {queryPlanner},
            );
            assert.gte(
                queryPlanner.optimizationTimeMicros,
                kSleepMs * 1000,
                "optimizationTimeMicros should be >= injected sleep delay in micros",
                {queryPlanner},
            );
        } finally {
            fp.off();
        }
    });

    it("reports optimizationTimeMillis >= 0 without a failpoint", function () {
        // Even without injected delay the field must exist and be non-negative.
        const explain = coll.explain().aggregate(pipeline);
        const queryPlanner = getQueryPlanner(explain);
        assert(
            queryPlanner.hasOwnProperty("optimizationTimeMillis"),
            "optimizationTimeMillis missing from queryPlanner",
            {queryPlanner},
        );
        assert.gte(
            queryPlanner.optimizationTimeMillis,
            0,
            "optimizationTimeMillis must be non-negative",
            {
                queryPlanner,
            },
        );
    });

    it("reports usedJoinOptimization:true so the timer is actually exercised", function () {
        // Confirm JOO was actually triggered (sanity-check that we're testing the right path).
        const explain = coll.explain().aggregate(pipeline);
        const winningPlan = getQueryPlanner(explain).winningPlan;
        assert(
            winningPlan.hasOwnProperty("usedJoinOptimization") && winningPlan.usedJoinOptimization,
            "expected join optimization to be used",
            {winningPlan},
        );
    });
});
