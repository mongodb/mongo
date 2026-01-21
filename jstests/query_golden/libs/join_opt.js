/**
 * Common helpers used in join optimization end-to-end tests.
 */
import {normalizeArray} from "jstests/libs/golden_test.js";
import {assertArrayEq, arrayEq} from "jstests/aggregation/extras/utils.js";
import {code, line, linebreak, subSection} from "jstests/libs/query/pretty_md.js";
import {getQueryPlanner} from "jstests/libs/query/analyze_plan.js";
import {prettyPrintWinningPlan} from "jstests/query_golden/libs/pretty_plan.js";

export function verifyExplainOutput(explain, joinOptExpectedInExplainOutput) {
    const winningPlan = getQueryPlanner(explain).winningPlan;

    if (joinOptExpectedInExplainOutput) {
        assert(winningPlan.hasOwnProperty("usedJoinOptimization") && winningPlan.usedJoinOptimization, winningPlan);
        // Golden tests utils don't output winningPlan stats so manually record it in this helper function.
        line(`usedJoinOptimization: ${winningPlan.usedJoinOptimization}`);
        linebreak();
        return;
    }

    // If the knob is not enabled, the explain should not include the join optimization flag.
    assert(!("usedJoinOptimization" in winningPlan), winningPlan);
}

export function getJoinTestResultsAndExplain(desc, coll, pipeline, params) {
    subSection(desc);
    assert.commandWorked(db.adminCommand({setParameter: 1, ...params}));
    return [coll.aggregate(pipeline).toArray(), coll.explain().aggregate(pipeline)];
}

/**
 * Note: if 'assertResultsEqual' is set to false, we just print a warning. This is useful for when we know a result divergence exists, but we should try to avoid using it unless we are planning on fixing this soon.
 */
export function runJoinTestAndCompare(desc, coll, pipeline, params, expected, assertResultsEqual = true) {
    const [actual, explain] = getJoinTestResultsAndExplain(desc, coll, pipeline, params);
    if (assertResultsEqual) {
        assertArrayEq({expected, actual});
    }
    verifyExplainOutput(explain, true /* joinOptExpectedInExplainOutput */);
    prettyPrintWinningPlan(explain);
    if (!assertResultsEqual) {
        if (!arrayEq(actual, expected)) {
            line("WARNING: results differ from expected!");
            subSection("Actual results");
            code(normalizeArray(actual, true /* shouldSortArray */));
        }
    }
}

/**
 * Restores join-opt parameters to state before test.
 */
export function joinTestWrapper(testFun) {
    const params = assert.commandWorked(
        db.adminCommand({
            getParameter: 1,
            internalEnableJoinOptimization: 1,
            internalJoinReorderMode: 1,
            internalJoinPlanTreeShape: 1,
            internalRandomJoinReorderDefaultToHashJoin: 1,
            internalMaxNodesInJoinGraph: 1,
            internalMaxEdgesInJoinGraph: 1,
            internalMaxNumberNodesConsideredForImplicitEdges: 1,
        }),
    );
    delete params.ok;

    try {
        testFun();
    } finally {
        assert.commandWorked(db.adminCommand({setParameter: 1, ...params}));
    }
}
