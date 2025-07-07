/**
 * Test exact cardinality mode of cost-based ranking.
 */

import {
    flattenPlan,
    getRejectedPlans,
    getWinningPlanFromExplain,
    isIndexOnly,
    planHasStage,
} from "jstests/libs/query/analyze_plan.js";
import {checkSbeFullyEnabled} from "jstests/libs/query/sbe_util.js";

if (checkSbeFullyEnabled(db)) {
    jsTestLog(`Skipping ${jsTestName()} as SBE executor is not supported yet`);
    quit();
}

const collName = jsTestName();
const coll = db.collName;
assert(coll.drop());

const kRandomValCeil = 5;
// Generates a random value between [0, kRandomValCeil).
function randomInt() {
    return Math.floor(Math.random() * kRandomValCeil);
}

// Helper to assert that each node in the winning plan has its cardinality correctly calculated.
function assertAllStagesCorrectCardinality(explain) {
    const flattenedPlan = flattenPlan(getWinningPlanFromExplain(explain));
    const flattenedExecution = flattenPlan(explain["executionStats"]["executionStages"]);
    assert.eq(flattenedPlan.length, flattenedExecution.length);
    for (let i = 0; i < flattenedPlan.length; i++) {
        assert.eq(flattenedPlan[i]["cardinalityEstimate"], flattenedExecution[i]["nReturned"]);
    }
}

// Helper to assert that we correctly calculate the cardinalities for a given query.
// This checks that the winning plan has the correct cardinality at every node,
// and checks that each rejected plan has the correct cardinality at the root node.
function assertCorrectCardinality({query, sort, skip, limit, project}) {
    let cmd = (project === undefined) ? coll.find(query) : coll.find(query, project);
    if (sort !== undefined) {
        cmd = cmd.sort(sort);
    }
    if (skip !== undefined) {
        cmd = cmd.skip(skip);
    }
    if (limit !== undefined) {
        cmd = cmd.limit(limit);
    }
    const explain = cmd.explain("executionStats");
    assertAllStagesCorrectCardinality(explain);
    const nReturned = explain["executionStats"]["nReturned"];
    getRejectedPlans(explain).forEach(plan => assert.eq(plan["cardinalityEstimate"], nReturned));
}

// This function asserts whether a given find query has a certain stage enumerated in a plan.
// This does not necessarily need to be in the winning plan, but should be somewhere.
function assertPlanEnumerated(query, stage) {
    const explain = coll.find(query).explain("executionStats");
    assert([getWinningPlanFromExplain(explain), ...getRejectedPlans(explain)].some(
        plan => planHasStage(db, plan, stage)));
}

function testAndHash() {
    // CBR might not choose the AND_HASH plan as the winning plan, so we check that this
    // plan is at least enumerated.
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryForceIntersectionPlans: true}));
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryPlannerEnableHashIntersection: true}));
    assert(coll.drop());
    assert.commandWorked(coll.createIndex({a: 1}));
    assert.commandWorked(coll.createIndex({b: 1}));
    assert.commandWorked(coll.createIndex({c: -1}));
    assert.commandWorked(coll.createIndex({e: 1}));
    assert.commandWorked(coll.createIndex({f: 1}));
    assert.commandWorked(coll.insertMany([
        {_id: 0, a: 1, b: "A", c: 22, d: "field"},
        {_id: 1, a: 1, b: "B", c: 22, d: "field"},
        {_id: 2, a: 1, b: "C", c: 22, d: "field"},
        {_id: 3, a: 2, b: 2, c: null},
        {_id: 4, a: 2, b: "D", c: 22},
        {_id: 5, a: 2, b: "E", c: 22, d: 99},
        {_id: 6, a: 3, b: "b", c: 23, d: "field"},
        {_id: 7, a: 3, b: "abc", c: 22, d: 23},
        {_id: 8, a: 3, b: "A", c: 22, d: "field"},
        {_id: 9, a: 4, b: "a", c: {x: 1, y: 2}, d: "field"},
        {_id: 10, a: 5, b: "ABC", d: 22, c: "field"},
        {_id: 11, a: 6, b: "ABC", d: 22, c: "field"},
        {_id: 12, a: 4, b: "a", c: {x: 1, y: 2}, d: "field", e: [1, 2, 3]},
        {_id: 13, a: 5, b: "ABC", d: 22, c: "field", e: [4, 5, 6]},
        {_id: 14, a: 6, b: "ABC", d: 22, c: "field", e: [7, 8, 9]},
        {_id: 15, a: 1, e: [7, 8, 9], f: [-1, -2, -3]},
    ]));
    const tests = [
        {a: {$gt: 1}, c: null},
        {a: {$gt: 3}, c: {x: 1, y: 2}},
        {a: {$lt: 5}, b: {$in: ["A", "abc"]}},
        {a: {$gt: 1}, e: {$elemMatch: {$lt: 7}}},
    ];
    tests.forEach(query => {
        assertPlanEnumerated(query, "AND_HASH");
        assertCorrectCardinality({query: query});
    });
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryForceIntersectionPlans: false}));
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryPlannerEnableHashIntersection: false}));
}

function testAndSorted() {
    // CBR might not choose the AND_SORTED plan as the winning plan, so we check that this
    // plan is at least enumerated.
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryForceIntersectionPlans: true}));
    assert(coll.drop());
    assert.commandWorked(coll.createIndex({a: 1}));
    assert.commandWorked(coll.createIndex({b: 1}));
    assert.commandWorked(coll.createIndex({c: -1}));

    assert.commandWorked(coll.insertMany([
        {_id: 0, a: 1, b: 1, c: 1},
        {_id: 1, a: 2, b: -1, c: 1},
        {_id: 2, a: 0, b: 0, c: 10},
        {_id: 3, a: 10, b: 1, c: -1}
    ]));
    assertPlanEnumerated({a: 1, b: 1}, "AND_SORTED");
    assertCorrectCardinality({a: 1, b: 1});
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryForceIntersectionPlans: false}));
}

function testRootedOr() {
    assert(coll.drop());
    for (let i = 0; i < 100; i++) {
        assert.commandWorked(coll.insert({a: i, b: i}));
    }
    assert.commandWorked(coll.createIndex({a: 1}));
    assert.commandWorked(coll.createIndex({b: 1}));
    assertCorrectCardinality({query: {$or: [{a: {$lt: 25}}, {b: {$gt: 75}}]}});
}

function testMergeSort() {
    assert(coll.drop());
    assert.commandWorked(
        coll.createIndexes([{a: 1, c: 1, d: 1}, {a: 1, c: -1, d: -1}, {c: 1, d: 1}]));

    for (let i = 0; i < 100; i++) {
        assert.commandWorked(
            coll.insert({a: randomInt(), b: randomInt(), c: randomInt(), d: randomInt()}));
    }
    assertCorrectCardinality({
        query: {$or: [{a: 4, b: 4}, {a: 3, b: 3}, {a: 2, b: 2}, {a: 1, b: 1}]},
        sort: {c: -1, d: -1}
    });
}

function testSort() {
    assert(coll.drop());
    for (let i = 0; i < 100; i++) {
        assert.commandWorked(coll.insert({a: randomInt(), b: randomInt()}));
    }
    assertCorrectCardinality({query: {a: 2}, sort: {b: 1}});
}

function testLimitSkip() {
    assert(coll.drop());
    for (let i = 0; i < 100; i++) {
        assert.commandWorked(coll.insert({a: i, b: i % 10}));
    }
    assertCorrectCardinality({query: {a: {$gt: 50}}, limit: 1});
    assertCorrectCardinality({query: {a: {$gt: 50}}, limit: 10});
    assertCorrectCardinality({query: {a: {$gt: 50}}, limit: 70});
    assertCorrectCardinality({query: {a: {$gt: 50}}, skip: 1});
    assertCorrectCardinality({query: {a: {$gt: 50}}, skip: 10});
    assertCorrectCardinality({query: {a: {$gt: 50}}, skip: 70});
}

function testCollIdxScan() {
    assert(coll.drop());
    for (let i = 0; i < 100; i++) {
        assert.commandWorked(coll.insert({a: i}));
    }
    assertCorrectCardinality({query: {a: {$lt: 50}}});
    assert.commandWorked(coll.createIndex({a: 1}));
    assertCorrectCardinality({query: {a: {$lt: 50}}});
}

function testFetch() {
    assert(coll.drop());
    for (let i = 0; i < 100; i++) {
        assert.commandWorked(coll.insert({a: i, b: i}));
    }
    assert.commandWorked(coll.createIndex({a: 1}));
    assertCorrectCardinality({query: {a: {$lt: 50}, b: 45}});
}

function testProjections() {
    assert(coll.drop());
    for (let i = 0; i < 100; i++) {
        assert.commandWorked(coll.insert({a: i, b: i}));
    }
    assertCorrectCardinality({project: {a: 1}});
    assertCorrectCardinality({query: {a: {$lt: 50}}, project: {b: 1}});
    assertCorrectCardinality({project: {b: 0}});
    assertCorrectCardinality({query: {a: {$lt: 50}}, project: {b: 0}});
    assertCorrectCardinality({query: {a: {$lt: 50}}, project: {c: {$add: ["$a", "$b"]}}});
}

function testCoveredPlans() {
    assert(coll.drop());
    for (let i = 0; i < 10; i++) {
        assert.commandWorked(coll.insert({a: i, b: i}));
    }
    assert.commandWorked(coll.createIndex({a: 1}));
    assert(isIndexOnly(
        db, getWinningPlanFromExplain(coll.find({a: {$lt: 5}}, {_id: 0, a: 1}).explain())));
    assertCorrectCardinality({query: {a: {$lt: 5}}, project: {_id: 0, a: 1}});

    assert.commandWorked(coll.dropIndex({a: 1}));
    assert.commandWorked(coll.createIndex({a: 1, b: 1}));
    [{query: {a: {$lt: 5}}, project: {a: 1, _id: 0}},
     {query: {a: {$lt: 5}, b: {$lt: 3}}, project: {a: 1, b: 1, _id: 0}}]
        .forEach(test => {
            assert(isIndexOnly(
                db, getWinningPlanFromExplain(coll.find(test.query, test.project).explain())));
            assertCorrectCardinality(test);
        });
}

try {
    assert.commandWorked(db.adminCommand({setParameter: 1, planRankerMode: "exactCE"}));
    // Ensure we calculate the correct cardinality for collection/index scans.
    testCollIdxScan();
    // Ensure we calculate the correct cardinality for fetches on top of index scans.
    testFetch();
    // Ensure we calculate the correct cardinality for index intersections using
    // AND_HASH/AND_SORTED.
    testAndHash();
    testAndSorted();
    // Ensure we calculate the correct cardinality for queries using a rooted OR.
    // TODO SERVER-97790: Once subplans are correctly annotated with CE data, we can uncomment this.
    // testRootedOr();
    // Ensure SORT_MERGE nodes are correctly calculated. Same TODO SERVER-97790 as above.
    // testMergeSort();
    // Ensure SORT nodes are correctly calculated.
    testSort();
    // Ensure LIMIT/SKIP nodes are correctly calculated.
    testLimitSkip();
    // Ensure projections are correctly calculated.
    testProjections();
    // Ensure covered plans are correctly calculated.
    testCoveredPlans();
} finally {
    // Ensure that query knob doesn't leak into other testcases in the suite.
    assert.commandWorked(db.adminCommand({setParameter: 1, planRankerMode: "multiPlanning"}));
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryForceIntersectionPlans: false}));
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryPlannerEnableHashIntersection: false}));
}
