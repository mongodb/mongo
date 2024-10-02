/**
 * Ensure that the correct CBR mode is chosen given certain combinations of query knobs.
 */

import {
    canonicalizePlan,
    getRejectedPlans,
    getWinningPlanFromExplain
} from "jstests/libs/analyze_plan.js";

import {checkSbeFullyEnabled} from "jstests/libs/sbe_util.js";

// TODO SERVER-92589: Remove this exemption
if (checkSbeFullyEnabled(db)) {
    jsTestLog(`Skipping ${jsTestName()} as SBE executor is not supported yet`);
    quit();
}

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
const q4 = {
    $or: [q1, q2]
};
const q5 = {
    $and: [
        {$or: [{a: 10}, {b: {$gt: 99}}]},
        {$or: [{a: {$in: [5, 1]}}, {b: {$in: [7, 99]}}]},
    ],
};

function assertCbrExplain(plan) {
    assert(plan.hasOwnProperty("cardinalityEstimate"));
    assert.gt(plan.cardinalityEstimate, 0);
    assert(plan.hasOwnProperty("costEstimate"));
    assert.gt(plan.costEstimate, 0);
    if (plan.hasOwnProperty("inputStage")) {
        assertCbrExplain(plan.inputStage);
    }
}

function checkLastRejectedPlan(query) {
    assert(!(Object.keys(query).length == 1 && Object.keys(query)[0] === "$or"),
           "encountered rooted $or query");

    // Classic plan via multiplanning
    assert.commandWorked(db.adminCommand({setParameter: 1, planRankerMode: "multiPlanning"}));
    const e0 = coll.find(query).explain();
    const r0 = getRejectedPlans(e0);
    // Validate there are rejected plans
    assert.gte(r0.length, 1);

    // Classic plan via CBR
    assert.commandWorked(db.adminCommand({setParameter: 1, planRankerMode: "automaticCE"}));
    const e1 = coll.find(query).explain();
    const w1 = getWinningPlanFromExplain(e1);
    assertCbrExplain(w1);
    const r1 = getRejectedPlans(e1);
    assert.gte(r1.length, 1);

    const lastMPRejectedPlan = r0[r0.length - 1];
    canonicalizePlan(lastMPRejectedPlan);
    canonicalizePlan(w1);
    assert.eq(lastMPRejectedPlan, w1);

    r1.map((e) => assertCbrExplain(e));
}

function checkRootedOr(query) {
    assert(Object.keys(query).length == 1 && Object.keys(query)[0] === "$or",
           "encountered non rooted $or query");

    // Plan via multiplanning
    assert.commandWorked(db.adminCommand({setParameter: 1, planRankerMode: "multiPlanning"}));
    const e0 = coll.find(query).explain();
    const w0 = getWinningPlanFromExplain(e0);

    // Plan via CBR
    assert.commandWorked(db.adminCommand({setParameter: 1, planRankerMode: "automaticCE"}));
    const e1 = coll.find(query).explain();
    const w1 = getWinningPlanFromExplain(e1);

    canonicalizePlan(w0);
    canonicalizePlan(w1);
    // Assert that the winning plans between multi-planning and CBR are different. This is the best
    // we can do for now since subplanning will discard the rejected plans for each branch.
    assert.neq(w0, w1);
}

try {
    checkLastRejectedPlan(q1);
    checkLastRejectedPlan(q2);
    checkLastRejectedPlan(q3);
    checkRootedOr(q4);
    checkLastRejectedPlan(q5);
} finally {
    // Ensure that query knob doesn't leak into other testcases in the suite.
    assert.commandWorked(db.adminCommand({setParameter: 1, planRankerMode: "multiPlanning"}));
}
