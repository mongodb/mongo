/**
 * Common utility functions variables for $group to DISTINCT_SCAN optimization.
 */

import {getAggPlanStages, getQueryPlanner} from "jstests/libs/analyze_plan.js";

export let coll;

// Add test data and indexes. Fields prefixed with "mk" are multikey.
export let indexList = [
    {pattern: {a: 1, b: 1, c: 1}, option: {}},
    {pattern: {mkA: 1, b: 1, c: 1}, option: {}},
    {pattern: {aa: 1, mkB: 1, c: 1}, option: {}},
    {pattern: {aa: 1, bb: 1, c: 1}, option: {}},
    {pattern: {"foo.a": 1, "foo.b": 1}, option: {}},
    {pattern: {"mkFoo.a": 1, "mkFoo.b": 1}, option: {}},
    {pattern: {"foo.a": 1, "mkFoo.b": 1}, option: {}}
];

export function createIndexes() {
    for (const indexSpec of indexList) {
        assert.commandWorked(coll.createIndex(indexSpec.pattern, indexSpec.option));
    }
}

export const documents = [
    {_id: 0, a: 1, b: 1, c: 1},
    {_id: 1, a: 1, b: 2, c: 2},
    {_id: 2, a: 1, b: 2, c: 3},
    {_id: 3, a: 1, b: 3, c: 2},
    {_id: 4, a: 2, b: 2, c: 2},
    {_id: 5, b: 1, c: 1},
    {_id: 6, a: null, b: 1, c: 1.5},

    {_id: 7, aa: 1, mkB: 2, bb: 2},
    {_id: 8, aa: 1, mkB: [1, 3], bb: 1},
    {_id: 9, aa: 2, mkB: [], bb: 3},

    {_id: 10, mkA: 1, c: 3},
    {_id: 11, mkA: [2, 3, 4], c: 3},
    {_id: 12, mkA: 2, c: 2},
    {_id: 13, mkA: 3, c: 4},

    {_id: 14, foo: {a: 1, b: 1}, mkFoo: {a: 1, b: 1}},
    {_id: 15, foo: {a: 1, b: 2}, mkFoo: {a: 1, b: 2}},
    {_id: 16, foo: {a: 2, b: 2}, mkFoo: {a: 2, b: 2}},
    {_id: 17, foo: {b: 1}, mkFoo: {b: 1}},
    {_id: 18, foo: {a: null, b: 1}, mkFoo: {a: null, b: 1}},
    {_id: 19, foo: {a: 3}, mkFoo: [{a: 3, b: 4}, {a: 4, b: 3}]},

    {_id: 20, str: "foo", d: 1},
    {_id: 21, str: "FoO", d: 2},
    {_id: 22, str: "bar", d: 4},
    {_id: 23, str: "bAr", d: 3}
];

// Helper for dropping an index and removing it from the list of indexes.
export function removeIndex(pattern) {
    assert.commandWorked(coll.dropIndex(pattern));
    indexList = indexList.filter((ix) => bsonWoCompare(ix.pattern, pattern) != 0);
}

export function addIndex(pattern, option) {
    indexList.push({pattern: pattern, option: option});
    assert.commandWorked(coll.createIndex(pattern, option));
}

// Prepare the 'coll' collection for testing, inserting documents and creating indexes.
export function prepareCollection(database = null) {
    database = database || db;
    coll = database[jsTestName()];
    assert(coll.drop());

    createIndexes();
    assert.commandWorked(coll.insert(documents));
}

// Check that 'pipeline' returns the correct results with and without a hint added to the query.
// We also test with and without indices to check all the possibilities. 'options' is the
// options to pass to aggregate() and may be omitted. Similarly, the hint object can be omitted
// and will default to a $natural hint.
export function assertResultsMatchWithAndWithoutHintandIndexes(pipeline,
                                                               expectedResults,
                                                               hintObj = {
                                                                   $natural: 1
                                                               },
                                                               options = {}) {
    assert.commandWorked(coll.dropIndexes());
    const resultsNoIndex = coll.aggregate(pipeline, options).toArray();

    createIndexes();
    const resultsWithIndex = coll.aggregate(pipeline, options).toArray();

    const passedOptions = Object.assign({}, {hint: hintObj}, options);
    const resultsWithHint = coll.aggregate(pipeline, passedOptions).toArray();

    assert.sameMembers(resultsNoIndex, resultsWithIndex, "no index != with index");
    assert.sameMembers(resultsWithIndex, resultsWithHint, "with index != with hint");
    assert.sameMembers(resultsWithHint, expectedResults, "with hint != expected");
}

export function assertPlanUsesDistinctScan(explain, keyPattern) {
    assert.neq(0, getAggPlanStages(explain, "DISTINCT_SCAN").length, explain);

    if (keyPattern) {
        assert.eq(keyPattern, getAggPlanStages(explain, "DISTINCT_SCAN")[0].keyPattern, explain);
    }

    // Pipelines that use the DISTINCT_SCAN optimization should not also have a blocking sort.
    assert.eq(0, getAggPlanStages(explain, "SORT").length, explain);
}

export function assertPlanDoesNotUseDistinctScan(explain) {
    assert.eq(0, getAggPlanStages(explain, "DISTINCT_SCAN").length, explain);
}

export function assertPlanUsesIndexScan(explain, keyPattern) {
    assertPlanDoesNotUseDistinctScan(explain);
    assert.neq(0, getAggPlanStages(explain, "IXSCAN").length, explain);
    assert.eq(keyPattern, getAggPlanStages(explain, "IXSCAN")[0].keyPattern);
}

export function assertPlanUsesCollScan(explain) {
    assertPlanDoesNotUseDistinctScan(explain);
    assert.eq(0, getAggPlanStages(explain, "IXSCAN").length, explain);
    assert.neq(0, getAggPlanStages(explain, "COLLSCAN").length, explain);
}

export function assertPipelineResultsAndExplain({
    pipeline,
    options = {},
    hint = undefined,
    expectsIndexFilter = false,
    expectedOutput,
    validateExplain,
}) {
    assertResultsMatchWithAndWithoutHintandIndexes(pipeline, expectedOutput, hint, options);
    const passedOptions = hint ? Object.assign({}, {hint}, options) : options;
    const explain = coll.explain().aggregate(pipeline, passedOptions);
    validateExplain(explain);
    if (expectsIndexFilter) {
        const queryPlanner = getQueryPlanner(explain);
        assert.eq(true, queryPlanner.indexFilterSet, queryPlanner);
    }
}
