/**
 * Utility functions for join optimization tests.
 */
import {getQueryPlanner} from "jstests/libs/query/analyze_plan.js";

// Runs the given test case with join optimization enabled and disabled, verifies that the results
// match expectedResults, and checks whether the join optimization was used as expected.
export function runTest({description, coll, pipeline, expectedResults, expectedUsedJoinOptimization}) {
    print(`Running test: ${description}`);
    assert.commandWorked(db.adminCommand({setParameter: 1, internalEnableJoinOptimization: false}));
    assert.eq(coll.aggregate(pipeline).toArray(), expectedResults);
    assert.commandWorked(db.adminCommand({setParameter: 1, internalEnableJoinOptimization: true}));
    assert.eq(coll.aggregate(pipeline).toArray(), expectedResults);

    const explain = coll.explain().aggregate(pipeline);
    print(`Explain: ${tojson(explain)}`);
    const winningPlan = getQueryPlanner(explain).winningPlan;
    assert.eq(expectedUsedJoinOptimization, winningPlan.usedJoinOptimization, winningPlan);
}
