/**
 * Ensure that the correct CBR mode is chosen given certain combinations of query knobs.
 */

import {getRejectedPlans, getWinningPlanFromExplain} from "jstests/libs/analyze_plan.js";

const collName = jsTestName();
const coll = db[collName];
coll.drop();

// Insert such data that some queries will do well with an {a: 1} index while
// others with a {b: 1} index.
assert.commandWorked(
    coll.insertMany(Array.from({length: 100}, (_, i) => ({a: 1, b: i, c: i % 7}))));
assert.commandWorked(
    coll.insertMany(Array.from({length: 100}, (_, i) => ({a: i, b: 1, c: i % 3}))));

coll.createIndexes([{a: 1}, {b: 1}, {c: 1, b: 1, a: 1}, {a: 1, b: 1}, {c: 1, a: 1}]);

// Queries designed in such a way that the winning plan is not the last enumerated plan.
// The current implementation of CBR chooses the last of all enumerated plans as winning.
// In this way we can verify that CBR was invoked by checking if the last rejected
// multi-planned plan is the winning plan.

const q1 = {
    a: {$gt: 10},
    b: {$eq: 99}
};
const q2 = {
    a: {$in: [5, 1]},
    b: {$in: [7, 99]}
};
const q3 = {
    a: {$gt: 90},
    b: {$eq: 99},
    c: {$lt: 5}
};

function checkLastRejectedPlan(query) {
    // Classic plan via multiplanning
    assert.commandWorked(db.adminCommand({setParameter: 1, planRankerMode: false}));
    const e0 = coll.find(query).explain();
    const r0 = getRejectedPlans(e0);
    // Validate there are rejected plans
    assert.gte(r0.length, 1);

    // Classic plan via CBR
    assert.commandWorked(db.adminCommand({setParameter: 1, planRankerMode: true}));
    const e1 = coll.find(query).explain();
    const w1 = getWinningPlanFromExplain(e1);

    const lastMPRejectedPlan = r0[r0.length - 1];

    // Explains of winning and rejected plans with featureFlagSbeFull differ by the 'isCached' and
    // 'planNodeId' fields. Remove them to allow us to compare.
    delete w1.planNodeId;
    delete w1.inputStage.planNodeId;
    delete w1.isCached;
    delete lastMPRejectedPlan.isCached;
    assert.eq(lastMPRejectedPlan, w1);
}

try {
    checkLastRejectedPlan(q1);
    checkLastRejectedPlan(q2);
    checkLastRejectedPlan(q3);
} finally {
    // Ensure that query knob doesn't leak into other testcases in the suite.
    assert.commandWorked(db.adminCommand({setParameter: 1, planRankerMode: false}));
}
