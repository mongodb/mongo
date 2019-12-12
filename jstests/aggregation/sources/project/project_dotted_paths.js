// Test that projection of dotted paths which happens in the "agg" layer works correctly. See
// SERVER-26066 for details.
(function() {
const coll = db.project_dotted_paths;
coll.drop();

assert.commandWorked(coll.insert({a: [1, {b: 2}, 3, {}]}));

function checkResultsConsistent(projection, expectedResults) {
    const aggResults = coll.aggregate([{$project: projection}]).toArray();
    const aggNoPushdownResults =
        coll.aggregate([{$_internalInhibitOptimization: {}}, {$project: projection}]).toArray();
    const findResults = coll.find({}, projection).toArray();

    assert.eq(aggResults, expectedResults);
    assert.eq(aggNoPushdownResults, expectedResults);
    assert.eq(findResults, expectedResults);
}

checkResultsConsistent({"a": {$literal: "newValue"}, _id: 0}, [{a: "newValue"}]);
checkResultsConsistent({"a.b": {$literal: "newValue"}, _id: 0},
                       [{a: [{b: "newValue"}, {b: "newValue"}, {b: "newValue"}, {b: "newValue"}]}]);
checkResultsConsistent({"a.b.c": {$literal: "newValue"}, _id: 0}, [
    {a: [{b: {c: "newValue"}}, {b: {c: "newValue"}}, {b: {c: "newValue"}}, {b: {c: "newValue"}}]}
]);
})();
