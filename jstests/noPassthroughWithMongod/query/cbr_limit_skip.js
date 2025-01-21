/**
 * Ensure limit and skip stages in find queries are estimated correctly.
 */

import {
    getPlanStage,
    getWinningPlanFromExplain,
} from "jstests/libs/query/analyze_plan.js";
import {ceEqual} from "jstests/libs/query/cbr_utils.js";
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
    docs.push({a: i, b: i % 10});
}
assert.commandWorked(coll.insert(docs));

coll.createIndexes([{a: 1}]);
assert.commandWorked(coll.runCommand({analyze: collName, key: "a"}));

function runTest({query, skip, limit, expectedCard}) {
    assert.commandWorked(db.adminCommand({setParameter: 1, planRankerMode: "histogramCE"}));
    let cmd = coll.find(query);
    if (skip !== null) {
        cmd = cmd.skip(skip);
    }
    if (limit !== null) {
        cmd = cmd.limit(limit);
    }
    const explain = cmd.explain();
    const plan = getWinningPlanFromExplain(explain);
    assert(plan.hasOwnProperty("cardinalityEstimate"), plan);
    assert(ceEqual(expectedCard, plan.cardinalityEstimate), plan);
}

try {
    runTest({query: {a: {$gt: 500}}, limit: 1, expectedCard: 1});
    runTest({query: {a: {$gt: 500}}, limit: 100, expectedCard: 100});
    runTest({query: {a: {$gt: 500}}, limit: 700, expectedCard: 499.4});
    runTest({query: {a: {$gt: 500}}, limit: 2000, expectedCard: 499.4});
    // Limit of 0 means don't apply a limit
    runTest({query: {a: {$gt: 500}}, limit: 0, expectedCard: 499.4});
    // Negative limit same effect as positive value
    runTest({query: {a: {$gt: 500}}, limit: -10, expectedCard: 10});
    runTest({query: {a: {$gt: 10000}}, limit: 5, expectedCard: 0});

    runTest({query: {a: {$gt: 500}}, skip: 1, expectedCard: 498.4});
    runTest({query: {a: {$gt: 500}}, skip: 100, expectedCard: 399.4});
    runTest({query: {a: {$gt: 500}}, skip: 500, expectedCard: 0});
    runTest({query: {a: {$gt: 10000}}, skip: 5, expectedCard: 0});
    runTest({query: {a: {$gt: 500}}, skip: 0, expectedCard: 499.4});

    runTest({query: {a: {$gt: 500}}, skip: 100, limit: 100, expectedCard: 100});
    runTest({query: {a: {$gt: 500}}, skip: 450, limit: 100, expectedCard: 49.4});
    runTest({query: {a: {$gt: 500}}, skip: 500, limit: 100, expectedCard: 0});
    runTest({query: {a: {$gt: 10000}}, skip: 100, limit: 100, expectedCard: 0});

    // TODO SERVER-99273: Support SORT stage with absorbed limit (top-K sort)
} finally {
    // Ensure that query knob doesn't leak into other testcases in the suite.
    assert.commandWorked(db.adminCommand({setParameter: 1, planRankerMode: "multiPlanning"}));
}

try {
    // Test to assert that skip node's cost is proportional to its input.
    coll.drop();
    const docs = Array.from({length: 100}, () => {
        return {a: 1};
    });
    assert.commandWorked(coll.insert(docs));
    coll.createIndexes([{a: 1}]);
    assert.commandWorked(coll.runCommand({analyze: collName, key: "a"}));

    assert.commandWorked(db.adminCommand({setParameter: 1, planRankerMode: "histogramCE"}));

    const query = {a: 1};
    const skip = 10;

    // Get the cost of the skip stage
    const smallCardSkipExplain = coll.find(query).skip(skip).explain();
    const smallCardSkipCost =
        getPlanStage(getWinningPlanFromExplain(smallCardSkipExplain), "SKIP").costEstimate;

    // Add more docs to the collection and update histogram
    assert.commandWorked(coll.insert(docs));
    assert.commandWorked(coll.runCommand({analyze: collName, key: "a"}));

    // Get the cost of the skip stage
    const largeCardSkipExplain = coll.find(query).skip(skip).explain();
    const largeCardSkipCost =
        getPlanStage(getWinningPlanFromExplain(largeCardSkipExplain), "SKIP").costEstimate;

    assert.gt(largeCardSkipCost, smallCardSkipCost);
} finally {
    // Ensure that query knob doesn't leak into other testcases in the suite.
    assert.commandWorked(db.adminCommand({setParameter: 1, planRankerMode: "multiPlanning"}));
}
