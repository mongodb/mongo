/**
 * Test sorting with dotted field paths and numeric path components.
 */
(function() {
"use strict";

const coll = db.sort_dotted_paths_positional;
coll.drop();

function testSortAndSortWithLimit(sortPattern, expectedIds) {
    assert.eq(expectedIds, coll.find({}, {_id: 1}).sort(sortPattern).toArray());
    assert.eq(expectedIds, coll.find({}, {_id: 1}).sort(sortPattern).limit(500).toArray());
}

// Basic tests to verify that sorting deals with undefined, null, missing fields, and nested arrays
// as expected.
assert.commandWorked(coll.insert([
    {_id: 1, a: 1},
    {_id: 2, a: undefined},
    {_id: 3, a: null},
    {_id: 4, a: {}},
    {_id: 5, a: []},
    {_id: 6, a: [1]},
    {_id: 7, a: [[1]]},
    {_id: 8},
    {_id: 9, a: [undefined]}
]));

// Test out sort({"a.0":1}) on a collection of documents where field 'a' is a mix of different
// types (arrays of varying size for some documents, non-arrays for other documents).
testSortAndSortWithLimit(
    {"a.0": 1, _id: 1},
    [{_id: 9}, {_id: 1}, {_id: 2}, {_id: 3}, {_id: 4}, {_id: 5}, {_id: 8}, {_id: 6}, {_id: 7}]);
testSortAndSortWithLimit(
    {"a.0": -1, _id: 1},
    [{_id: 7}, {_id: 6}, {_id: 1}, {_id: 2}, {_id: 3}, {_id: 4}, {_id: 5}, {_id: 8}, {_id: 9}]);

// More tests for a sort pattern that contains two paths of length 2 with a common prefix.
assert(coll.drop());
assert.commandWorked(coll.insert([
    {_id: 1, a: [{b: [4, 1], c: 1}, {b: [1, 5], c: 2}]},
    {_id: 2, a: [{b: 2, c: [1, 3]}, {b: 4, c: [2, 4]}]},
    {_id: 3, a: [{b: [6, 4], c: 1}, {b: [9, 7], c: 2}]},
    {_id: 4, a: [{b: 4, c: [1, 2]}, {b: 5, c: [3, 2]}]},
    {_id: 5, a: [{b: [2, 3], c: 1}, {b: [7, 6], c: 3}]},
    {_id: 6, a: [{b: 5, c: []}, {b: 3, c: 3}]},
    {_id: 7, a: [{b: [], c: 2}, {b: 2, c: 3}]},
    {_id: 8, a: [{b: 3, c: [2]}, {b: 8, c: [3]}]},
    {_id: 9, a: [{b: [8], c: 2}, {b: [6], c: 3}]},
]));

// Test out sort({"a.0.b":1}) on a collection of documents where field "a" and sub-field "b" are
// a mix of different types.
testSortAndSortWithLimit(
    {"a.0.b": 1, _id: 1},
    [{_id: 7}, {_id: 1}, {_id: 2}, {_id: 5}, {_id: 8}, {_id: 3}, {_id: 4}, {_id: 6}, {_id: 9}]);
testSortAndSortWithLimit(
    {"a.0.b": 1, _id: -1},
    [{_id: 7}, {_id: 1}, {_id: 5}, {_id: 2}, {_id: 8}, {_id: 4}, {_id: 3}, {_id: 6}, {_id: 9}]);
testSortAndSortWithLimit(
    {"a.0.b": -1, _id: 1},
    [{_id: 9}, {_id: 3}, {_id: 6}, {_id: 1}, {_id: 4}, {_id: 5}, {_id: 8}, {_id: 2}, {_id: 7}]);
testSortAndSortWithLimit(
    {"a.0.b": -1, _id: -1},
    [{_id: 9}, {_id: 3}, {_id: 6}, {_id: 4}, {_id: 1}, {_id: 8}, {_id: 5}, {_id: 2}, {_id: 7}]);
})();
