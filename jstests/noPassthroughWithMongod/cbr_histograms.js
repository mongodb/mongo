/**
 * Ensure that the analyze command produces histograms which cost-based ranking is able to use.
 */

import {
    canonicalizePlan,
    getRejectedPlans,
    getWinningPlanFromExplain,
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
        docs.push({a: j, b: i % 3});
    }
}
assert.commandWorked(coll.insertMany(docs));

coll.createIndex({a: 1});

// Generate histograms for field 'a' and 'b'
assert.commandWorked(coll.runCommand({analyze: collName, key: "a", numberBuckets: 10}));
assert.commandWorked(coll.runCommand({analyze: collName, key: "b"}));

// Run a find command with the given filter and verify that every plan uses a histogram estimate.
function assertQueryUsesHistograms(query) {
    const explain = coll.find(query).explain();
    [getWinningPlanFromExplain(explain), ...getRejectedPlans(explain)].forEach(plan => {
        assert.eq(plan.estimatesMetadata.ceSource, "Histogram", plan);
    });
}

try {
    // Use histogram CE
    assert.commandWorked(db.adminCommand({setParameter: 1, planRankerMode: "histogramCE"}));
    const testCases = [
        // IndexScan should use histogram
        {a: 5},
        {a: {$gt: 5}},
        {a: {$lt: 5}},
        // CollScan with sargable filter should use histogram
        {b: 5},
        {b: {$gt: 5}},
        {b: {$lt: 5}},
    ];
    testCases.forEach(tc => assertQueryUsesHistograms(tc));
} finally {
    // Ensure that query knob doesn't leak into other testcases in the suite.
    assert.commandWorked(db.adminCommand({setParameter: 1, planRankerMode: "multiPlanning"}));
}
