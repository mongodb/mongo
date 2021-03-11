// Tests for whether the query solution correctly used an AND_SORTED stage for index intersection.
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");  // For assertArrayEq.
load("jstests/libs/analyze_plan.js");  // For planHasStage helper to analyze explain() output.

const conn = MongoRunner.runMongod();
const db = conn.getDB("test");

// Enable and force index intersections plans.
assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryForceIntersectionPlans: true}));

function runAndSortedTests() {
    const coll = db.and_sorted;
    coll.drop();

    assert.commandWorked(coll.createIndex({a: 1}));
    assert.commandWorked(coll.createIndex({b: 1}));
    assert.commandWorked(coll.createIndex({c: -1}));

    assert.commandWorked(coll.insertMany([
        {_id: 0, a: 1, b: 1, c: 1},
        {_id: 1, a: 2, b: -1, c: 1},
        {_id: 2, a: 0, b: 0, c: 10},
        {_id: 3, a: 10, b: 1, c: -1}
    ]));

    // Helper to check the result returned by the query and to check whether the query solution
    // correctly did or did not use an AND_SORTED for index intersection.
    function assertAndSortedUsed({query, expectedResult, shouldUseAndSorted} = {}) {
        const queryResult = coll.find(query);
        const expl = queryResult.explain();

        assertArrayEq({actual: queryResult.toArray(), expected: expectedResult});
        assert.eq(shouldUseAndSorted,
                  planHasStage(db, getWinningPlan(expl.queryPlanner), "AND_SORTED"));
    }

    // Test basic index intersection where we expect AND_SORTED to be used.
    assertAndSortedUsed({
        query: {a: 1, b: 1},
        expectedResult: [{_id: 0, a: 1, b: 1, c: 1}],
        shouldUseAndSorted: true
    });

    assert.commandWorked(coll.insertMany([
        {_id: 4, a: 100, b: 100, c: 1},
        {_id: 5, a: 100, b: 100, c: 2},
        {_id: 6, a: 100, b: 100, c: 3},
        {_id: 7, a: 100, b: 100, c: 1}
    ]));

    assertAndSortedUsed({
        query: {a: 100, c: 1},
        expectedResult: [{_id: 4, a: 100, b: 100, c: 1}, {_id: 7, a: 100, b: 100, c: 1}],
        shouldUseAndSorted: true
    });
    assertAndSortedUsed({
        query: {a: 100, b: 100, c: 1},
        expectedResult: [{_id: 4, a: 100, b: 100, c: 1}, {_id: 7, a: 100, b: 100, c: 1}],
        shouldUseAndSorted: true
    });
    assertAndSortedUsed({
        query: {a: 100, b: 100, c: 2},
        expectedResult: [{_id: 5, a: 100, b: 100, c: 2}],
        shouldUseAndSorted: true
    });

    assert.commandWorked(coll.insertMany(
        [{_id: 8, c: 1, d: 1, e: 1}, {_id: 9, c: 1, d: 2, e: 2}, {_id: 10, c: 1, d: 2, e: 3}]));
    assert.commandWorked(coll.createIndex({e: 1}));

    // Test where shouldn't use AND_SORTED since no index exists on one of the fields or the query
    // is on a single field.
    assertAndSortedUsed({
        query: {c: 1, d: 2},
        expectedResult: [{_id: 9, c: 1, d: 2, e: 2}, {_id: 10, c: 1, d: 2, e: 3}],
        shouldUseAndSorted: false
    });
    assertAndSortedUsed(
        {query: {e: 1}, expectedResult: [{_id: 8, c: 1, d: 1, e: 1}], shouldUseAndSorted: false});

    // Test on an empty collection.
    assert(coll.drop());
    assert.commandWorked(coll.createIndex({a: 1}));
    assert.commandWorked(coll.createIndex({b: 1}));

    assertAndSortedUsed({query: {a: 1, b: 1}, expectedResult: [], shouldUseAndSorted: true});

    // Test more than two branches.
    assert(coll.drop());
    assert.commandWorked(coll.insertMany([
        {_id: 1, a: 1, b: 2, c: 5, d: 9, e: 5},
        {_id: 2, a: 1, b: 2, c: 3, d: 4, e: 5},
        {_id: 3, a: 1, b: 2, c: 3, d: 4, e: 6},
        {_id: 4, a: 1, b: 4, c: 3, d: 4, e: 5}
    ]));
    assert.commandWorked(coll.createIndex({a: 1}));
    assert.commandWorked(coll.createIndex({b: 1}));
    assert.commandWorked(coll.createIndex({c: 1}));
    assert.commandWorked(coll.createIndex({d: 1}));
    assert.commandWorked(coll.createIndex({e: 1}));

    assertAndSortedUsed({
        query: {a: 1, b: 2, c: 3, d: 4, e: 5},
        expectedResult: [{_id: 2, a: 1, b: 2, c: 3, d: 4, e: 5}],
        shouldUseAndSorted: true
    });

    // Test with arrays, strings, and non-scalar predicates.
    assert(coll.drop());
    assert.commandWorked(coll.insertMany([
        {_id: 1, a: 1, b: [1, 2, 3], c: "c"},
        {_id: 2, a: [1, 2, 3], b: 2, c: "c"},
        {_id: 3, a: 2, b: "b", c: ["a", "b", "c"]}
    ]));
    assert.commandWorked(coll.createIndex({a: 1}));
    assert.commandWorked(coll.createIndex({b: 1}));
    assert.commandWorked(coll.createIndex({c: 1}));

    assertAndSortedUsed({
        query: {a: 1, c: "c"},
        expectedResult:
            [{_id: 1, a: 1, b: [1, 2, 3], c: "c"}, {_id: 2, a: [1, 2, 3], b: 2, c: "c"}],
        shouldUseAndSorted: true
    });
    assertAndSortedUsed({
        query: {a: 1, b: 2},
        expectedResult:
            [{_id: 1, a: 1, b: [1, 2, 3], c: "c"}, {_id: 2, a: [1, 2, 3], b: 2, c: "c"}],
        shouldUseAndSorted: true
    });
    assertAndSortedUsed({
        query: {a: 2, c: "c"},
        expectedResult:
            [{_id: 2, a: [1, 2, 3], b: 2, c: "c"}, {_id: 3, a: 2, b: "b", c: ["a", "b", "c"]}],
        shouldUseAndSorted: true
    });
    assertAndSortedUsed({
        query: {a: 2, c: {"$size": 3}},
        expectedResult: [{_id: 3, a: 2, b: "b", c: ["a", "b", "c"]}],
        shouldUseAndSorted: false
    });
}

runAndSortedTests();

// Re-run the tests now with 'internalQueryExecYieldIterations' set to '1' such that yield happens
// after each document is returned.
assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryExecYieldIterations: 1}));
runAndSortedTests();

MongoRunner.stopMongod(conn);
})();
