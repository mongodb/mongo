// Ensure that sort behavior for undefined, missing, and null fields is the same for both find and
// aggregation. This test validates the fix for SERVER-42565, which was caused by inconsistent
// behavior for generating sort keys in aggregation.
// @tags: [
//   sbe_incompatible,
// ]
(function() {
"use strict";

const coll = db.sort_key_with_missing_fields;
coll.drop();

assert.commandWorked(coll.insert([
    {_id: 1, a: undefined},
    {_id: 2, a: null},
    {_id: 3, a: []},
    {_id: 4},
    {_id: 5},
    {_id: 6, a: []},
    {_id: 7, a: null},
    {_id: 8, a: undefined},
]));

// Correct ordering treats {a: undefined} and {a: []} as the same value (undefined) and treats {a:
// null} and missing "a" as the same value (null). When we sort by the "a" field, we expect the sort
// to put all the undefined-like documents in one group (1, 3, 6, 8) and all null-like documents in
// another (2, 4, 5, 7). We break the ties within these two groups by sorting on the "_id" field.
const expectedOrderForward = [1, 3, 6, 8, 2, 4, 5, 7];
const expectedOrderReverse = [...expectedOrderForward].reverse();

function getArrayOfIds(arrayOfDocs) {
    return arrayOfDocs.map(doc => doc._id);
}

function checkFindAndAggSorts(sortPattern, expectedOrder) {
    // We include the sort key in the projection so that it will be included in the diagnostic
    // output if the test fails as a result of incorrect output.
    const findResult =
        coll.find({}, {_id: 1, a: 1, key: {$meta: "sortKey"}}).sort(sortPattern).toArray();
    assert.eq(getArrayOfIds(findResult), expectedOrder, findResult);

    const aggResult =
        coll.aggregate({$sort: sortPattern}, {$addFields: {key: {$meta: "sortKey"}}}).toArray();
    assert.eq(getArrayOfIds(aggResult), expectedOrder, aggResult);
}

checkFindAndAggSorts({a: 1, _id: 1}, expectedOrderForward);
checkFindAndAggSorts({a: -1, _id: -1}, expectedOrderReverse);

// Repeat the above test, but this time we sort on a path that traverse an array, forcing the sort
// key generator to use its general path, instead of the fast path for sorting on non-array paths.
assert(coll.drop());
assert.commandWorked(coll.insert([
    {_id: 1, a: [{b: undefined}]},
    {_id: 2, a: [{b: null}]},
    {_id: 3, a: [{b: []}]},
    {_id: 4, a: [{}]},
    {_id: 5},
    {_id: 6, a: [{b: []}]},
    {_id: 7, a: [{b: null}]},
    {_id: 8, a: [{b: undefined}]},
]));

checkFindAndAggSorts({"a.b": 1, _id: 1}, expectedOrderForward);
checkFindAndAggSorts({"a.b": -1, _id: -1}, expectedOrderReverse);
}());