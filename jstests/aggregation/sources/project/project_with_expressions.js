/**
 * Test that a $project with a combination of expressions and field projections gets evaluted
 * correctly, and overwrites the data present in the input document when necessary.
 */
const coll = db.project_with_expressions;
coll.drop();

assert.commandWorked(coll.insert({_id: 0, a: {subObj1: {p: 1}, subObj2: {p: 1}, subObj3: {p: 1}}}));

function assertProjectionResultForFindAndAgg(projection, expectedResults) {
    const aggResults = coll.aggregate([{$project: projection}]).toArray();
    const aggNoPushdownResults = coll
        .aggregate([{$_internalInhibitOptimization: {}}, {$project: projection}])
        .toArray();
    const findResults = coll.find({}, projection).toArray();

    // SERVER-54314 The equality checks below don't make assertions about field ordering in order to
    // account for differences between the classic engine and SBE. In particular, the classic engine
    // will apply inclusions before applying computed projections, whereas SBE will apply the
    // projections in the order in which the fields appear in the original document.
    function assertResultsEqual(results, expected) {
        assert.eq(results.length, expected.length);
        for (let i = 0; i < results.length; ++i) {
            assert.docEq(expected[i], results[i]);
        }
    }
    assertResultsEqual(aggResults, expectedResults);
    assertResultsEqual(aggNoPushdownResults, expectedResults);
    assertResultsEqual(findResults, expectedResults);
}

// Case where a project with a valid sub-object, a project with missing sub-object and a project
// with sub-expression share a common parent, and the projection is represented using a dotted path.
assertProjectionResultForFindAndAgg({_id: 0, "a.subObj1": {$literal: 1}, "a.subObj2": 1, "a.subObj3.q": 1}, [
    {a: {subObj2: {p: 1}, subObj3: {}, subObj1: 1}},
]);
assertProjectionResultForFindAndAgg({_id: 0, "a.subObj2": 1, "a.subObj1": {$literal: 1}, "a.subObj3.q": {r: 1}}, [
    {a: {subObj2: {p: 1}, subObj3: {}, subObj1: 1}},
]);

// Case where a project with a valid sub-object, a project with missing sub-object and a project
// with sub-expression share a common parent, and the projection is represented using sub-objects.
assertProjectionResultForFindAndAgg({_id: 0, a: {subObj1: {$literal: 1}, subObj2: 1, subObj3: {q: {r: 1}}}}, [
    {a: {subObj2: {p: 1}, subObj3: {}, subObj1: 1}},
]);
assertProjectionResultForFindAndAgg({_id: 0, a: {subObj2: 1, subObj1: {$literal: 1}, subObj3: {q: 1}}}, [
    {a: {subObj2: {p: 1}, subObj3: {}, subObj1: 1}},
]);
