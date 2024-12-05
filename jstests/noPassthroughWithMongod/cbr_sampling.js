/**
 * Test that cost-based ranking can use sampling to estimate filters.
 */

import {
    getWinningPlanFromExplain,
    isCollscan,
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

let docs = [];
for (let i = 0; i < 1000; i++) {
    docs.push({a: i, b: [i - 1, i, i + 1]});
}

assert.commandWorked(coll.insert(docs));

function assertCollscanUsesSampling(query) {
    const explain = coll.find(query).explain();
    const plan = getWinningPlanFromExplain(explain);
    assert(isCollscan(db, plan));
    assert.eq(plan.estimatesMetadata.ceSource, "Sampling", plan);
}

try {
    assert.commandWorked(db.adminCommand({setParameter: 1, planRankerMode: "samplingCE"}));
    assertCollscanUsesSampling({a: {$lt: 100}});
    assertCollscanUsesSampling({b: {$elemMatch: {$gt: 100, $lt: 200}}});
    // TODO SERVER-97790: Enable rooted $or test
    // assertCollscanUsesSampling({$or: [{a: {$lt: 100}}, {b: {$gt: 900}}]});
} finally {
    // Ensure that query knob doesn't leak into other testcases in the suite.
    assert.commandWorked(db.adminCommand({setParameter: 1, planRankerMode: "multiPlanning"}));
}
