/**
 * Ensure that the correct CBR mode is chosen given certain combinations of query knobs.
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

// Insert such data that some queries will do well with an {a: 1} index while
// others with a {b: 1} index.
assert.commandWorked(
    coll.insertMany(Array.from({length: 1000}, (_, i) => ({a: 1, b: i, c: i % 7}))));
assert.commandWorked(
    coll.insertMany(Array.from({length: 1000}, (_, i) => ({a: i, b: 1, c: i % 3}))));

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

function verifyCollectionCardinalityEstimate() {
    const card = 1234;
    coll.drop();
    assert.commandWorked(coll.insertMany(Array.from({length: card}, () => ({a: 1}))));
    assert.commandWorked(db.adminCommand({setParameter: 1, planRankerMode: "automaticCE"}));
    // This query should not have any predicates, as they are taken into account
    // by CE, and estimated cardinality will be less than the total.
    const e1 = coll.find({}).explain();
    const w1 = getWinningPlanFromExplain(e1);
    assert(isCollscan(db, w1));
    assert.eq(w1.cardinalityEstimate, card);
}

function verifyHeuristicEstimateSource() {
    coll.drop();
    assert.commandWorked(coll.insert({a: 1}));
    assert.commandWorked(coll.createIndex({a: 1}));
    assert.commandWorked(db.adminCommand({setParameter: 1, planRankerMode: "heuristicCE"}));
    const e1 = coll.find({a: 1}).explain();
    const w1 = getWinningPlanFromExplain(e1);
    assert.eq(w1.estimatesMetadata.ceSource, "Heuristics", w1);
}

try {
    checkLastRejectedPlan(q1);
    checkLastRejectedPlan(q2);
    checkLastRejectedPlan(q3);
    checkRootedOr(q4);
    checkLastRejectedPlan(q5);
    verifyCollectionCardinalityEstimate();
    verifyHeuristicEstimateSource();

    /**
     * Test strict and automatic CE modes.
     */

    // With strict mode Histogram CE without an applicable histogram should produce
    // an error. Automatic mode should result in heuristicCE or mixedCE.

    // TODO SERVER-97867: Since in automaticCE mode we always fallback to heuristic CE,
    // it is not possible to ever fallback to multi-planning.

    coll.drop();
    assert.commandWorked(coll.insertOne({a: 1}));

    assert.commandWorked(db.adminCommand({setParameter: 1, planRankerMode: "histogramCE"}));

    // Request histogam CE while the collection has no histogram
    assert.throwsWithCode(() => coll.find({a: 1}).explain(), ErrorCodes.HistogramCEFailure);

    // Create a histogram on field "b".
    // TODO SERVER-97814: Due to incompleteness of CBR 'analyze' must be run with multi-planning.
    assert.commandWorked(db.adminCommand({setParameter: 1, planRankerMode: "multiPlanning"}));
    assert.commandWorked(coll.runCommand({analyze: collName, key: "b"}));
    assert.commandWorked(db.adminCommand({setParameter: 1, planRankerMode: "histogramCE"}));

    // Request histogam CE on a field that doesn't have a histogram
    assert.throwsWithCode(() => coll.find({a: 1}).explain(), ErrorCodes.HistogramCEFailure);
    assert.throwsWithCode(() => coll.find({$and: [{b: 1}, {a: 3}]}).explain(),
                          ErrorCodes.HistogramCEFailure);

    // $or cannot fail because QueryPlanner::planSubqueries() falls back to choosePlanWholeQuery()
    // when one of the subqueries could not be planned. In this way the CE error is masked.
    const orExpl = coll.find({$or: [{b: 1}, {a: 3}]}).explain();
    assert(isCollscan(db, getWinningPlanFromExplain(orExpl)));

    // Histogram CE invokes conversion of expression to an inexact interval, which fails
    assert.throwsWithCode(() => coll.find({b: {$gt: []}}).explain(), ErrorCodes.HistogramCEFailure);

    // Histogram CE fails because of inestimable interval
    assert.throwsWithCode(() => coll.find({b: {$gte: {foo: 1}}}).explain(),
                          ErrorCodes.HistogramCEFailure);
} finally {
    // Ensure that query knob doesn't leak into other testcases in the suite.
    assert.commandWorked(db.adminCommand({setParameter: 1, planRankerMode: "multiPlanning"}));
}
