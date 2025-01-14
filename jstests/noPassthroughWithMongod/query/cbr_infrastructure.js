/**
 * Ensure that the correct CBR mode is chosen given certain combinations of query knobs.
 */

import {
    canonicalizePlan,
    getExecutionStats,
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
const collName1 = collName + "_ceModes";
const coll = db[collName];
const coll1 = db[collName1];
coll.drop();
coll1.drop();

// Insert such data that some queries will do well with an {a: 1} index while
// others with a {b: 1} index.
assert.commandWorked(coll.insertMany(Array.from({length: 3000}, (_, i) => {
    const doc = {a: 1, b: i, c: i % 7, x: i % 5};
    if (i % 9 === 0) {
        doc.missing_90_percent = i;
    }
    return doc;
})));
assert.commandWorked(coll.insertMany(Array.from({length: 3000}, (_, i) => {
    const doc = {a: i, b: 1, c: i % 3, x: i % 9};
    if (i % 9 === 0) {
        doc.missing_90_percent = i % 3;
    }
    return doc;
})));

coll.createIndexes(
    [{a: 1}, {b: 1}, {c: 1, b: 1, a: 1}, {a: 1, b: 1}, {c: 1, a: 1}, {missing_90_percent: 1}]);

assert.commandWorked(coll.runCommand({analyze: collName, key: "a", numberBuckets: 10}));
assert.commandWorked(coll.runCommand({analyze: collName, key: "b", numberBuckets: 10}));
assert.commandWorked(coll.runCommand({analyze: collName, key: "c", numberBuckets: 10}));
assert.commandWorked(
    coll.runCommand({analyze: collName, key: "missing_90_percent", numberBuckets: 10}));

// Test queries
const queries = [
    {a: {$gt: 10}, b: {$eq: 99}},
    {a: {$in: [5, 1]}, b: {$in: [7, 99]}},
    {a: {$gt: 90}, b: {$eq: 99}, c: {$lt: 5}},
    /*
    The following query has 4 plans:
    1. Filter: a in (1,5) AND b in (7, 99)
       Or
       |  Ixscan: b: [99, inf]
       Ixscan: a: [10, 10], b: [MinKey, MaxKey]

    2. Filter: a in (1,5) AND b in (7, 99)
       Or
       |  Ixscan: b: [99, inf]
       Ixscan: a: [10, 10]

    3. Filter: a = 10 AND b > 99
       Or
       |  Ixscan: a: [1,1]U[5,5]
       Ixscan: b: [7,7]U[99,99]

    4. Filter: a = 10 AND b > 99
       Or
       |  Ixscan: a: [1,1] U [5,5] b: [MinKey, MaxKey]
       Ixscan: b: [7,7] U [99,99]

    In classic, plan 1 is the winner and in CBR, the winner is plan 2. In CBR we cost plans 1 and
    2 to have equal costs. In reality, plan 2 is better because it uses an index with a shorter key
    length, but multiplanning is unable to distinguish that because of it's early exit behavior. So
    asserting that CBR choose the same plan as classic won't always work because CBR's plan may be
    better. Therefore, instead of comparing plans the test compares the number of keys and documents
    scanned by a plan. CBR plans should scan no more than Classic plans.
    */
    {
        $and: [
            {$or: [{a: 10}, {b: {$gt: 99}}]},
            {$or: [{a: {$in: [5, 1]}}, {b: {$in: [7, 99]}}]},
        ],
    },
    {a: {$not: {$lt: 130}}},
    {missing_90_percent: {$exists: false}},
    {missing_90_percent: {$not: {$exists: false}}},
    {a: {$not: {$lt: 130}}, b: 12, c: {$not: {$gt: 1200}}},
    {a: {$not: {$in: [[100, 101, 102]]}}},
    {$nor: [{$and: [{a: {$lt: 10}}, {b: {$gt: 19}}]}]},
    {$nor: [{$or: [{a: {$lt: 10}}, {b: {$gt: 19}}]}]},
    {$nor: [{b: {$ne: 3}}]},
    {$nor: [{a: {$ne: 1}}]},
    {$alwaysFalse: 1},
    {$alwaysTrue: 1},
    {a: {$in: []}},
    {a: {$all: []}},
    {a: {$gt: 10}, b: {$in: []}},
    {$nor: [{a: 1}]},
    {$nor: [{a: 1}, {b: {$gt: 1000}}]},
    {$and: [{$nor: [{a: 1}, {a: {$gt: 1000}}]}, {b: {$lt: 100}}]},
    {$and: [{$or: [{$nor: [{a: {$gt: 100}}, {b: {$gt: 50}}]}, {a: 1}]}, {b: {$lt: 100}}]},
];

queries.push({$or: [queries[0], queries[1]]});

function assertCbrExplain(plan) {
    assert(plan.hasOwnProperty("cardinalityEstimate"), plan);
    if (plan.stage === "EOF") {
        assert.eq(plan.cardinalityEstimate, 0, plan);
    } else {
        assert.gt(plan.cardinalityEstimate, 0, plan);
    }
    assert(plan.hasOwnProperty("costEstimate"), plan);
    assert.gt(plan.costEstimate, 0, plan);
    if (plan.hasOwnProperty("inputStage")) {
        assertCbrExplain(plan.inputStage);
    } else if (plan.hasOwnProperty("inputStages")) {
        plan.inputStages.forEach(p => assertCbrExplain(p));
    } else {
        assert(plan.hasOwnProperty("numKeysEstimate") || plan.hasOwnProperty("numDocsEstimate"),
               plan);
    }
}

function checkWinningPlan({query = {}, project = {}, order = {}}) {
    const isRootedOr = (Object.keys(query).length == 1 && Object.keys(query)[0] === "$or");

    // Classic plan via multiplanning
    assert.commandWorked(db.adminCommand({setParameter: 1, planRankerMode: "multiPlanning"}));
    const e0 = coll.find(query, project).sort(order).explain("executionStats");
    const w0 = getWinningPlanFromExplain(e0);
    const r0 = getRejectedPlans(e0);

    // Classic plan via CBR
    assert.commandWorked(db.adminCommand({setParameter: 1, planRankerMode: "automaticCE"}));
    const e1 = coll.find(query, project).sort(order).explain("executionStats");
    const w1 = getWinningPlanFromExplain(e1);
    const r1 = getRejectedPlans(e1);

    if (!isRootedOr) {
        assertCbrExplain(w1);
        // Both explains must have the same number of rejected plans
        assert.eq(r0.length, r1.length);
    }
    r1.map((e) => assertCbrExplain(e));

    // CBR's plan must be no worse than the Classic plan
    assert(e1.executionStats.totalKeysExamined <= e0.executionStats.totalKeysExamined);
    assert(e1.executionStats.totalDocsExamined <= e0.executionStats.totalDocsExamined);
    assert(e1.executionStats.executionStages.works <= e0.executionStats.executionStages.works);
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
    assertCbrExplain(w1);
    assert.eq(w1.cardinalityEstimate, card);
}

function verifyHeuristicEstimateSource() {
    coll.drop();
    assert.commandWorked(coll.insert({a: 1}));
    assert.commandWorked(coll.createIndex({a: 1}));
    assert.commandWorked(db.adminCommand({setParameter: 1, planRankerMode: "heuristicCE"}));
    const e1 = coll.find({a: 1}).explain();
    const w1 = getWinningPlanFromExplain(e1);
    assertCbrExplain(w1);
    assert.eq(w1.estimatesMetadata.ceSource, "Heuristics", w1);
}

try {
    for (const q of queries) {
        checkWinningPlan({query: q});
        // Test variants of each query with different combinations of projection and sort fields.
        // The tests permute which field is indexed, and whether the indexed fields are the same
        // or different ones.
        checkWinningPlan({query: q, project: {a: 1}});
        checkWinningPlan({query: q, project: {x: 1}});
        checkWinningPlan({query: q, project: {a: 1, _id: 0}});
        checkWinningPlan({query: q, project: {x: 1, _id: 0}});
        checkWinningPlan({query: q, project: {c: 0}});
        checkWinningPlan({query: q, project: {a: 1, b: 1}});
        checkWinningPlan({query: q, project: {a: 1, x: 1}});
        checkWinningPlan({query: q, project: {b: {$add: ['$a', 1]}}});
        checkWinningPlan({query: q, project: {a: 1}, order: {a: 1}});
        checkWinningPlan({query: q, project: {a: 1}, order: {b: 1}});
        checkWinningPlan({query: q, project: {x: 1}, order: {a: 1}});
        checkWinningPlan({query: q, project: {a: 1}, order: {x: 1}});
        checkWinningPlan({query: q, project: {x: 1}, order: {x: 1}});
        checkWinningPlan({query: q, project: {a: 1}, order: {b: -1, x: -1}});
        checkWinningPlan({query: q, project: {a: 1, b: 1}, order: {c: 1, b: 1, a: 1}});
    }

    verifyCollectionCardinalityEstimate();
    verifyHeuristicEstimateSource();

    /**
     * Test strict and automatic CE modes.
     */

    // With strict mode Histogram CE without an applicable histogram should produce
    // an error. Automatic mode should result in heuristicCE or mixedCE.

    // TODO SERVER-97867: Since in automaticCE mode we always fallback to heuristic CE,
    // it is not possible to ever fallback to multi-planning.

    assert.commandWorked(coll1.insertOne({a: 1}));

    assert.commandWorked(db.adminCommand({setParameter: 1, planRankerMode: "histogramCE"}));

    // Request histogam CE while the collection has no histogram
    assert.throwsWithCode(() => coll1.find({a: 1}).explain(), ErrorCodes.HistogramCEFailure);

    // Create a histogram on field "b".
    assert.commandWorked(coll1.runCommand({analyze: collName, key: "b"}));

    // Request histogam CE on a field that doesn't have a histogram
    assert.throwsWithCode(() => coll1.find({a: 1}).explain(), ErrorCodes.HistogramCEFailure);
    assert.throwsWithCode(() => coll1.find({$and: [{b: 1}, {a: 3}]}).explain(),
                          ErrorCodes.HistogramCEFailure);

    // $or cannot fail because QueryPlanner::planSubqueries() falls back to choosePlanWholeQuery()
    // when one of the subqueries could not be planned. In this way the CE error is masked.
    const orExpl = coll1.find({$or: [{b: 1}, {a: 3}]}).explain();
    assert(isCollscan(db, getWinningPlanFromExplain(orExpl)));

    // Histogram CE invokes conversion of expression to an inexact interval, which fails
    assert.throwsWithCode(() => coll1.find({b: {$gt: []}}).explain(),
                          ErrorCodes.HistogramCEFailure);

    // Histogram CE fails because of inestimable interval
    assert.throwsWithCode(() => coll1.find({b: {$gte: {foo: 1}}}).explain(),
                          ErrorCodes.HistogramCEFailure);
} finally {
    // Ensure that query knob doesn't leak into other testcases in the suite.
    assert.commandWorked(db.adminCommand({setParameter: 1, planRankerMode: "multiPlanning"}));
}
