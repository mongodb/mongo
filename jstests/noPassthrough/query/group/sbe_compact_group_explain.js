/**
 * Tests that SBE runs an eligible $group with the compact_group stage when
 * featureFlagSbeCompactHashAgg is enabled and disk use is not allowed.
 *
 * @tags: [
 *   requires_fcv_91,
 * ]
 */

import {getEngine, getNestedProperties, getQueryPlanners} from "jstests/libs/query/analyze_plan.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";

describe("Compact hash aggregation stage selection and execution", function () {
    let conn;
    let coll;

    before(function () {
        conn = MongoRunner.runMongod({
            setParameter: {
                featureFlagSbeCompactHashAgg: true,
                internalQueryFrameworkControl: "trySbeEngine",
            },
        });
        assert.neq(conn, null, "mongod failed to start");
        coll = conn.getDB(jsTestName()).coll;
        assert.commandWorked(
            coll.insertMany([
                {k: 1, x: 2},
                {k: 2, x: 3},
                {k: 1, x: 4},
            ]),
        );
    });

    after(function () {
        if (conn) {
            MongoRunner.stopMongod(conn);
            conn = null;
        }
    });

    function usesCompactAggregation(pipeline, allowDiskUse) {
        const explain = coll.explain().aggregate(pipeline, {allowDiskUse});
        assert.eq("sbe", getEngine(explain), explain);
        return getQueryPlanners(explain)
            .flatMap((queryPlanner) => getNestedProperties(queryPlanner, "slotBasedPlan"))
            .some((plan) => plan.stages.includes("compact_group"));
    }

    function usesCompactGroup(accumulators, allowDiskUse) {
        return usesCompactAggregation([{$group: {_id: "$k", ...accumulators}}], allowDiskUse);
    }

    it("uses compact aggregation for $sum", function () {
        assert(usesCompactGroup({total: {$sum: "$x"}}, false));
    });

    it("uses compact aggregation for $push", function () {
        assert(usesCompactGroup({xs: {$push: "$x"}}, false));
    });

    it("uses compact aggregation for $count after $match", function () {
        const pipeline = [{$match: {k: 1}}, {$count: "n"}];
        assert(usesCompactAggregation(pipeline, false));
        assert.eq([{n: 2}], coll.aggregate(pipeline, {allowDiskUse: false}).toArray());
    });

    it("uses compact aggregation without accumulators", function () {
        assert(usesCompactGroup({}, false));
    });

    it("falls back to hash aggregation when disk use is allowed", function () {
        assert(!usesCompactGroup({total: {$sum: "$x"}}, true));
    });

    it("falls back to hash aggregation for unsupported accumulators", function () {
        assert(!usesCompactGroup({top: {$max: "$x"}}, false));
    });

    it("executes compact $sum and $push accumulators", function () {
        assert.eq(
            [
                {_id: 1, xs: [2, 4], total: 6},
                {_id: 2, xs: [3], total: 3},
            ],
            coll
                .aggregate(
                    [
                        {$group: {_id: "$k", xs: {$push: "$x"}, total: {$sum: "$x"}}},
                        {$sort: {_id: 1}},
                    ],
                    {allowDiskUse: false},
                )
                .toArray(),
        );
    });
});
