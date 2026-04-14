/**
 * Test explain output (especially 'rejectedPlans') for join optimization.
 *
 * @tags: [
 *   requires_fcv_90,
 *   requires_sbe,
 *   featureFlagPathArrayness
 * ]
 */

import {linebreak, section, subSection} from "jstests/libs/query/pretty_md.js";
import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {getRejectedPlans} from "jstests/libs/query/analyze_plan.js";
import {
    prettyPrintWinningPlan,
    prettyPrintRejectedPlans,
    getWinningJoinOrderOneLine,
} from "jstests/query_golden/libs/pretty_plan.js";
import {getJoinTestResultsAndExplain, verifyExplainOutput} from "jstests/query_golden/libs/join_opt.js";
import {joinTestWrapper} from "jstests/libs/query/join_utils.js";

const coll = db[jsTestName()];
coll.drop();
assert.commandWorked(coll.insertMany([{_id: 0, a: 1}]));
// Add index for multikeyness info for path arrayness.
assert.commandWorked(coll.createIndex({dummy: 1, a: 1}));

// Create a fully-connected 4-node join (after implicit edges are added in).
const pipeline = [
    {$lookup: {from: coll.getName(), localField: "a", foreignField: "a", as: "a1"}},
    {$unwind: "$a1"},
    {$lookup: {from: coll.getName(), localField: "a1.a", foreignField: "a", as: "a2"}},
    {$unwind: "$a2"},
    {$lookup: {from: coll.getName(), localField: "a2.a", foreignField: "a", as: "a3"}},
    {$unwind: "$a3"},
];

let expectedWinningOrder = undefined;
function testAllPlans(desc, baseRes, minLevel, maxLevel) {
    const [actual, explain] = getJoinTestResultsAndExplain(desc, coll, pipeline, {
        setParameter: 1,
        internalEnableJoinOptimization: true,
        internalJoinReorderMode: "bottomUp",
        internalJoinPlanTreeShape: "zigZag",
        internalMinAllPlansEnumerationSubsetLevel: minLevel,
        internalMaxAllPlansEnumerationSubsetLevel: maxLevel,
    });

    assertArrayEq({expected: baseRes, actual});
    verifyExplainOutput(explain, true /* joinOptExpectedInExplainOutput */);
    if (!expectedWinningOrder) {
        subSection("Winning plan");
        prettyPrintWinningPlan(explain);
        // Initialize the first (cheapest) order.
        expectedWinningOrder = getWinningJoinOrderOneLine(explain);
    } else {
        // Ensure we pick the same plan regardless of enumeration strategy.
        assert.eq(getWinningJoinOrderOneLine(explain), expectedWinningOrder);
    }

    const numRejected = getRejectedPlans(explain).length;
    subSection(`Rejected plans (total: ${numRejected})`);
    prettyPrintRejectedPlans(explain, 3);

    linebreak();
}

joinTestWrapper(db, () => {
    assert.commandWorked(db.adminCommand({setParameter: 1, internalEnableJoinOptimization: false}));
    const baseRes = coll.aggregate(pipeline).toArray();

    section("Test getting rejected plan out of explain");
    testAllPlans("Cheapest plan (no ALL plans enum)", baseRes, 64, 64);
    testAllPlans("ALL plans, subset level 0 only", baseRes, 0, 1);
    testAllPlans("ALL plans, subset level 1 only ", baseRes, 1, 2);
    testAllPlans("ALL plans, subset level 2 only ", baseRes, 2, 3);
    testAllPlans("ALL plans, subset level 3 only ", baseRes, 3, 4);
    testAllPlans("ALL plans, all levels", baseRes, 0, 4);
});
