/**
 * Test that CBR falls back to multiplanning when a plan contains an index scan whose cardinality
 * estimate is zero. CBR does not reliably pick the better plan when costs rest on a zero estimate,
 * so the candidate plans are handed to the multi-planner instead.
 *
 * @tags: [
 *   requires_fcv_90,
 * ]
 */

import {
    assertChosenRanker,
    ChosenRanker,
    getPlanStage,
    getWinningPlanFromExplain,
    PlanRankerReason,
} from "jstests/libs/query/analyze_plan.js";
import {getPlanRankerConfig, setPlanRankerConfig} from "jstests/libs/query/cbr_utils.js";
import {checkSbeFullyEnabled} from "jstests/libs/query/sbe_util.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";

// TODO SERVER-117707: Remove this check once CBR is enabled for SBE
if (checkSbeFullyEnabled(db)) {
    jsTestLog(`Skipping ${jsTestName()} as SBE executor is not supported yet`);
    quit();
}

const coll = db[jsTestName()];

// The query below matches nothing, so its two candidate index scans tie on multi-planner score
// and the tie is broken by candidate order: the planner enumerates the {a: 1} index first.
const zeroCEFilter = {a: {$gt: 5000}, b: {$gt: 5000}};

function winningIndex(explain) {
    const ixscan = getPlanStage(getWinningPlanFromExplain(explain), "IXSCAN");
    assert.neq(null, ixscan, "expected an IXSCAN in the winning plan", {explain});
    return ixscan.keyPattern;
}

describe("CBR with a zero cardinality estimate", function () {
    let prevPlanRankerConfig;
    let prevPlanEvaluationWorks;

    before(function () {
        coll.drop();
        // 1,000 docs with a = b = 0..999. Two indexes so that the queries below have multiple
        // candidate plans and plan ranking is engaged. 'g' is present on the second half of the
        // collection only: it is common enough to be observed by the sample, yet absent from the
        // prefix of the {a: 1} index that the multi-planner's trial phase scans.
        assert.commandWorked(
            coll.insertMany(
                Array.from({length: 1000}, (_, i) => (i < 500 ? {a: i, b: i} : {a: i, b: i, g: 1})),
            ),
        );
        assert.commandWorked(coll.createIndex({a: 1}));
        assert.commandWorked(coll.createIndex({b: 1}));

        prevPlanRankerConfig = getPlanRankerConfig(db);
        // "mixed" so that the multi-planner runs first and only hands over to CBR when its trial
        // phase produces no results - the path this test is about.
        setPlanRankerConfig(db, {
            internalQueryPlanRanker: "mixed",
            internalQueryCBRCEMode: "samplingCE",
            internalQueryMixedPlanRankingStrategy: "NoMultiplanningResults",
        });
        // Keep the multi-planner's trial phase short so it gives up before reaching the documents
        // carrying 'g' in the control query below.
        prevPlanEvaluationWorks = assert.commandWorked(
            db.adminCommand({setParameter: 1, internalQueryPlanEvaluationWorks: 10}),
        ).was;
    });

    after(function () {
        setPlanRankerConfig(db, prevPlanRankerConfig);
        assert.commandWorked(
            db.adminCommand({
                setParameter: 1,
                internalQueryPlanEvaluationWorks: prevPlanEvaluationWorks,
            }),
        );
        coll.drop();
    });

    it("hands every candidate to the multi-planner in its original order", function () {
        coll.getPlanCache().clear();

        // No document (and hence no sampled document) matches, so every candidate plan has a zero
        // cardinality estimate. CBR must not trust the zero estimates and must hand all plans to
        // the multi-planner.
        const zeroCEExplain = coll.find(zeroCEFilter).explain();
        assertChosenRanker(
            zeroCEExplain,
            ChosenRanker.kMultiPlanning,
            PlanRankerReason.kCBRInestimableNode,
        );

        // Abandoning CBR must hand the candidates to the multi-planner in the planner's original
        // order, so the outcome is exactly what multi-planning alone would have produced: the tie
        // is broken in favour of the first enumerated index, {a: 1}.
        assert.eq(
            {a: 1},
            winningIndex(zeroCEExplain),
            "zero-CE fallback changed the winning plan",
            {
                zeroCEExplain,
            },
        );
    });

    it("ranks with CBR when the sample observes matching documents", function () {
        coll.getPlanCache().clear();

        // The multi-planner also hands over here - the trial phase scans the first half of the
        // {a: 1} index, none of which has 'g', and exhausts its works budget without a result -
        // but this time the sample does observe matching documents, so the estimates are non-zero
        // and CBR ranks the plans itself.
        const nonZeroCEExplain = coll.find({a: {$gte: 0}, b: {$gte: 0}, g: 1}).explain();
        assertChosenRanker(
            nonZeroCEExplain,
            ChosenRanker.kCostBased,
            PlanRankerReason.kNoMultiplanningResults,
        );
    });
});
