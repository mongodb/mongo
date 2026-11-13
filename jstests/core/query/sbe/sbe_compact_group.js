/**
 * Tests that a $group that SBE runs with CompactHashAggStage returns the same results as one that
 * runs with HashAggStage, and that the compact stage is chosen only when disk use is not allowed.
 * Numeric edge cases are covered separately by CompactHashAggAccumulatorTest.
 *
 * @tags: [
 *   uses_explain,
 *   assumes_read_concern_local,
 *   assumes_read_concern_unchanged,
 *   does_not_support_transactions,
 *   requires_fcv_91,
 * ]
 */

import {getNestedProperties, getQueryPlanners} from "jstests/libs/query/analyze_plan.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";

const coll = db.sbe_compact_group;

function sbeStages(explain) {
    return getQueryPlanners(explain)
        .flatMap((queryPlanner) => getNestedProperties(queryPlanner, "slotBasedPlan"))
        .map((plan) => plan.stages)
        .join("\n");
}

function sortedById(results) {
    return results.sort((lhs, rhs) => bsonWoCompare({_id: lhs._id}, {_id: rhs._id}));
}

describe("$group with CompactHashAggStage", function () {
    let compactFlag;

    function runGroup(pipeline, options = {}) {
        const compact = sortedById(
            coll.aggregate(pipeline, {...options, allowDiskUse: false}).toArray(),
        );
        const reference = sortedById(
            coll.aggregate(pipeline, {...options, allowDiskUse: true}).toArray(),
        );
        assert.eq(compact.length, reference.length, {compact, reference});
        for (let i = 0; i < compact.length; ++i) {
            assert(bsonBinaryEqual(compact[i], reference[i]), {
                pipeline,
                compact: compact[i],
                reference: reference[i],
            });
        }

        const compactStages = sbeStages(
            coll.explain().aggregate(pipeline, {...options, allowDiskUse: false}),
        );
        const referenceStages = sbeStages(
            coll.explain().aggregate(pipeline, {...options, allowDiskUse: true}),
        );
        assert(!referenceStages.includes("compact_group"), referenceStages);
        if (compactFlag && /\bgroup \[/.test(referenceStages)) {
            assert(compactStages.includes("compact_group"), compactStages);
        }
        if (!compactFlag) {
            assert(!compactStages.includes("compact_group"), compactStages);
        }
    }

    before(function () {
        coll.drop();
        assert.commandWorked(
            coll.insertMany([
                {_id: 0, k: 1, s: "a", x: 2, y: 10},
                {_id: 1, k: 1, s: "A", x: 2, y: 20},
                {_id: 2, k: 2, s: "b", x: 3, y: 10},
                {_id: 3, k: 3, s: "B", x: 4, y: 30},
            ]),
        );
        compactFlag = FeatureFlagUtil.isPresentAndEnabled(db, "SbeCompactHashAgg");
    });

    after(function () {
        coll.drop();
    });

    it("sums a field by a scalar key", function () {
        runGroup([{$group: {_id: "$k", total: {$sum: "$x"}}}]);
    });

    it("computes two sums", function () {
        runGroup([{$group: {_id: "$k", x: {$sum: "$x"}, y: {$sum: "$y"}}}]);
    });

    it("groups by a compound key", function () {
        runGroup([{$group: {_id: {k: "$k", x: "$x"}, total: {$sum: "$y"}}}]);
    });

    it("groups every document into one group", function () {
        runGroup([{$group: {_id: null, total: {$sum: "$y"}}}]);
    });

    it("groups without accumulators", function () {
        runGroup([{$group: {_id: "$k"}}]);
    });

    it("groups strings under a case-insensitive collation", function () {
        runGroup(
            [
                {$group: {_id: "$s", total: {$sum: "$y"}}},
                {$project: {_id: {$toLower: "$_id"}, total: 1}},
            ],
            {collation: {locale: "en", strength: 2}},
        );
    });

    it("groups after a $match", function () {
        runGroup([{$match: {k: {$lt: 3}}}, {$group: {_id: "$y", total: {$sum: "$x"}}}]);
    });

    it("pushes values alongside a sum", function () {
        runGroup([
            {$group: {_id: "$k", ys: {$push: "$y"}, total: {$sum: "$y"}}},
            {$project: {total: 1, ys: {$sortArray: {input: "$ys", sortBy: 1}}}},
        ]);
    });
});
