/**
 * Test that CBR falls back to multiplanning for a plan containing an index scan that sampling
 * estimates to scan zero keys. A zero estimate from sampling typically means "no matching data
 * observed in the sample" rather than "truly zero", so the plan's cost cannot be trusted and the
 * plan is handed to the multi-planner instead.
 *
 * @tags: [
 *   requires_fcv_90,
 * ]
 */

import {
    assertChosenRanker,
    ChosenRanker,
    getPlanStage,
    getRejectedPlans,
    getWinningPlanFromExplain,
} from "jstests/libs/query/analyze_plan.js";
import {
    getPlanRankerConfig,
    isPlanCosted,
    setPlanRankerConfig,
} from "jstests/libs/query/cbr_utils.js";
import {checkSbeFullyEnabled} from "jstests/libs/query/sbe_util.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";

// TODO SERVER-117707: Remove this check once CBR is enabled for SBE
if (checkSbeFullyEnabled(db)) {
    jsTestLog(`Skipping ${jsTestName()} as SBE executor is not supported yet`);
    quit();
}

const coll = db[jsTestName()];

function scannedIndex(plan) {
    const ixscan = getPlanStage(plan, "IXSCAN");
    assert.neq(null, ixscan, "expected an IXSCAN in the plan", {plan});
    return ixscan.keyPattern;
}

function winningIndex(explain) {
    return scannedIndex(getWinningPlanFromExplain(explain));
}

function allPlans(explain) {
    return [getWinningPlanFromExplain(explain), ...getRejectedPlans(explain)];
}

describe("CBR with a zero cardinality estimate", function () {
    let prevPlanRankerConfig;

    before(function () {
        coll.drop();
        // 1,000 docs with a = b = c = 0..999, and indexes so that the queries below have multiple
        // candidate plans and plan ranking is engaged.
        assert.commandWorked(
            coll.insertMany(Array.from({length: 1000}, (_, i) => ({a: i, b: i, c: i}))),
        );
        assert.commandWorked(coll.createIndex({a: 1}));
        assert.commandWorked(coll.createIndex({b: 1}));
        assert.commandWorked(coll.createIndex({c: 1}));

        prevPlanRankerConfig = getPlanRankerConfig(db);
        // Strict cost-based ranking, so that CBR ranks every query before any multi-planning trial:
        // the multi-planner only runs on the plans CBR hands to it. An uncosted plan is therefore
        // one CBR could not estimate.
        setPlanRankerConfig(db, {
            internalQueryPlanRanker: "costBased",
            internalQueryCBRCEMode: "samplingCE",
        });
    });

    after(function () {
        setPlanRankerConfig(db, prevPlanRankerConfig);
        coll.drop();
    });

    it("hands every candidate to the multi-planner in its original order", function () {
        coll.getPlanCache().clear();

        // No document (and hence no sampled document) matches, so both candidate index scans are
        // estimated to scan zero keys. CBR must not trust the zero estimates: it estimates neither
        // plan and hands both to the multi-planner, which picks the winner.
        const zeroCEExplain = coll.find({a: {$gt: 5000}, b: {$gt: 5000}}).explain();
        assertChosenRanker(zeroCEExplain, ChosenRanker.kMultiPlanning);
        for (const plan of allPlans(zeroCEExplain)) {
            assert(!isPlanCosted(plan), "expected CBR not to cost a zero-CE plan", {plan});
        }

        // The plans are handed to the multi-planner in the planner's original order. They tie on
        // score, as neither returns a document, so the tie is broken in favour of the first
        // enumerated index, {a: 1}.
        assert.eq(
            {a: 1},
            winningIndex(zeroCEExplain),
            "zero-CE fallback changed the winning plan",
            {
                zeroCEExplain,
            },
        );
    });

    it("still ranks the plans whose index scans do not estimate zero keys", function () {
        coll.getPlanCache().clear();

        // Only the plan scanning {a: 1} is estimated to scan zero keys: 'a > 5000' matches nothing,
        // while the 'b' and 'c' predicates match most of the collection. CBR still costs the plans
        // scanning {b: 1} and {c: 1}, and hands only the cheaper of them to the multi-planner along
        // with the zero-CE plan.
        const partialZeroCEExplain = coll
            .find({a: {$gt: 5000}, b: {$gt: 5}, c: {$gt: 5}})
            .explain();
        assertChosenRanker(partialZeroCEExplain, ChosenRanker.kMultiPlanning);

        // The zero-CE plan wins the multi-planner's trial, as it reaches EOF at once, and is not
        // costed.
        const winningPlan = getWinningPlanFromExplain(partialZeroCEExplain);
        assert.eq({a: 1}, scannedIndex(winningPlan), "unexpected winning plan", {
            partialZeroCEExplain,
        });
        assert(!isPlanCosted(winningPlan), "expected CBR not to cost the zero-CE plan", {
            winningPlan,
        });

        // Both estimable plans are rejected with the cost CBR gave them: one by the multi-planner,
        // the other by CBR itself.
        const rejectedPlans = getRejectedPlans(partialZeroCEExplain);
        assert.sameMembers(
            [{b: 1}, {c: 1}],
            rejectedPlans.map((plan) => scannedIndex(plan)),
            "unexpected rejected plans",
            undefined /* compareFn */,
            {partialZeroCEExplain},
        );
        for (const plan of rejectedPlans) {
            assert(isPlanCosted(plan), "expected CBR to cost an estimable plan", {plan});
        }
    });
});
