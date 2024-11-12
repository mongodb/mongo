/**
 * Ensure that the analyze command produces histograms which cost-based ranking is able to use.
 */

import {
    canonicalizePlan,
    getRejectedPlans,
    getWinningPlanFromExplain,
    isCollscan
} from "jstests/libs/query/analyze_plan.js";

import {checkSbeFullyEnabled} from "jstests/libs/query/sbe_util.js";

// TODO SERVER-92589: Remove this exemption
if (checkSbeFullyEnabled(db)) {
    jsTestLog(`Skipping ${jsTestName()} as SBE executor is not supported yet`);
    quit();
}

const collName = jsTestName();
const coll = db[collName];
coll.drop();

// Generate docs with decreasing distribution
let docs = [];
for (let i = 0; i < 100; i++) {
    for (let j = 0; j < i; j++) {
        docs.push({a: j});
    }
}
assert.commandWorked(coll.insertMany(docs));

coll.createIndex({a: 1});

// Generate histogram for field 'a'
assert.commandWorked(coll.runCommand({analyze: collName, key: "a", numberBuckets: 10}));

try {
    // Use histogram CE
    assert.commandWorked(db.adminCommand({setParameter: 1, planRankerMode: "histogramCE"}));
    const explain = coll.find({a: 5}).explain();
    assert.eq(0, getRejectedPlans(explain).length);
    const winningPlan = getWinningPlanFromExplain(explain);
    assert.eq(winningPlan.estimatesMetadata.ceSource, "Histogram", winningPlan);
} finally {
    // Ensure that query knob doesn't leak into other testcases in the suite.
    assert.commandWorked(db.adminCommand({setParameter: 1, planRankerMode: "multiPlanning"}));
}
