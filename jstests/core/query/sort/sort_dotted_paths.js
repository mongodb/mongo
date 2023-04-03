/**
 * Test sorting with dotted field paths.
 */
(function() {
"use strict";

const coll = db.sort_dotted_paths;
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

testSortAndSortWithLimit(
    {a: 1, _id: 1},
    [{_id: 2}, {_id: 5}, {_id: 9}, {_id: 3}, {_id: 8}, {_id: 1}, {_id: 6}, {_id: 4}, {_id: 7}]);
testSortAndSortWithLimit(
    {a: -1, _id: 1},
    [{_id: 7}, {_id: 4}, {_id: 1}, {_id: 6}, {_id: 3}, {_id: 8}, {_id: 2}, {_id: 5}, {_id: 9}]);

// Test out sort({a:1,b:1}) on a collection where a is an array for some documents and b is an array
// for other documents.
assert(coll.drop());
assert.commandWorked(coll.insert([
    {_id: 1, a: 1, b: [2]},
    {_id: 2, a: 2, b: []},
    {_id: 3, a: [], b: 4},
    {_id: 4, a: [1], b: 4},
    {_id: 5, a: [2, 3], b: 2},
    {_id: 6, a: 2, b: [3, 5]},
    {_id: 7, a: 1, b: [1, 3]},
    {_id: 8, a: [1, 2], b: 3},
    {_id: 9, a: 2, b: 3}
]));

testSortAndSortWithLimit(
    {a: 1, b: 1, _id: 1},
    [{_id: 3}, {_id: 7}, {_id: 1}, {_id: 8}, {_id: 4}, {_id: 2}, {_id: 5}, {_id: 6}, {_id: 9}]);
testSortAndSortWithLimit(
    {a: 1, b: -1, _id: 1},
    [{_id: 3}, {_id: 4}, {_id: 7}, {_id: 8}, {_id: 1}, {_id: 6}, {_id: 9}, {_id: 5}, {_id: 2}]);
testSortAndSortWithLimit(
    {a: -1, b: 1, _id: 1},
    [{_id: 5}, {_id: 2}, {_id: 6}, {_id: 8}, {_id: 9}, {_id: 7}, {_id: 1}, {_id: 4}, {_id: 3}]);
testSortAndSortWithLimit(
    {a: -1, b: -1, _id: 1},
    [{_id: 5}, {_id: 6}, {_id: 8}, {_id: 9}, {_id: 2}, {_id: 4}, {_id: 7}, {_id: 1}, {_id: 3}]);

assert(coll.drop());
assert.commandWorked(
    coll.insert([{_id: 1, a: [], b: 1}, {_id: 2, a: 1, b: []}, {_id: 3, a: [], b: []}]));

// Verify that sort({a:1,b:1}) does not fail with a "parallel arrays" error when documents where
// both a and b are arrays are filtered out.
const filter1 = {
    $or: [{a: {$not: {$type: "array"}}}, {b: {$not: {$type: "array"}}}]
};
const output1 = [{_id: 1, a: [], b: 1}, {_id: 2, a: 1, b: []}];
assert.eq(coll.find(filter1).sort({a: 1, b: 1}).toArray(), output1);
assert.eq(coll.find(filter1).sort({_id: 1, a: 1, b: 1}).toArray(), output1);
assert.eq(coll.find(filter1).sort({a: 1, _id: 1, b: 1}).toArray(), output1);
assert.eq(coll.find(filter1).sort({a: 1, b: 1, _id: 1}).toArray(), output1);

// Basic tests for a sort pattern that contains a path of length 2.
assert(coll.drop());
assert.commandWorked(coll.insert([
    {_id: 1, a: {b: 2}},
    {_id: 2, a: {b: []}},
    {_id: 3, a: {b: [2, 3]}},
    {_id: 4, a: {c: 1}},
    {_id: 5, a: []},
    {_id: 6, a: [{b: [5]}]},
    {_id: 7, a: [{b: [4, 5]}, {b: 7}]},
    {_id: 8, a: [{b: [1, 3]}, {b: [2, 5]}]},
    {_id: 9, a: [{b: []}, {b: [1, 3]}]}
]));

testSortAndSortWithLimit(
    {"a.b": 1, _id: 1},
    [{_id: 2}, {_id: 9}, {_id: 4}, {_id: 5}, {_id: 8}, {_id: 1}, {_id: 3}, {_id: 7}, {_id: 6}]);
testSortAndSortWithLimit(
    {"a.b": 1, _id: -1},
    [{_id: 9}, {_id: 2}, {_id: 5}, {_id: 4}, {_id: 8}, {_id: 3}, {_id: 1}, {_id: 7}, {_id: 6}]);
testSortAndSortWithLimit(
    {"a.b": -1, _id: 1},
    [{_id: 7}, {_id: 6}, {_id: 8}, {_id: 3}, {_id: 9}, {_id: 1}, {_id: 4}, {_id: 5}, {_id: 2}]);
testSortAndSortWithLimit(
    {"a.b": -1, _id: -1},
    [{_id: 7}, {_id: 8}, {_id: 6}, {_id: 9}, {_id: 3}, {_id: 1}, {_id: 5}, {_id: 4}, {_id: 2}]);

// Basic tests for a sort pattern that contains two paths of length 2 with a common prefix.
assert(coll.drop());
assert.commandWorked(coll.insert([
    {_id: 1, a: [{b: 4, c: 1}, {b: 1, c: 2}]},
    {_id: 2, a: [{b: 2, c: 1}, {b: 4, c: 2}]},
    {_id: 3, a: [{b: 6, c: 1}, {b: 9, c: 2}]},
    {_id: 4, a: [{b: 4, c: 1}, {b: 5, c: 3}]},
    {_id: 5, a: [{b: 2, c: 1}, {b: 7, c: 3}]},
    {_id: 6, a: [{b: 5, c: 1}, {b: 3, c: 3}]},
    {_id: 7, a: [{b: 7, c: 2}, {b: 2, c: 3}]},
    {_id: 8, a: [{b: 3, c: 2}, {b: 8, c: 3}]},
    {_id: 9, a: [{b: 8, c: 2}, {b: 6, c: 3}]},
]));

testSortAndSortWithLimit(
    {"a.c": 1, "a.b": 1, _id: 1},
    [{_id: 2}, {_id: 5}, {_id: 1}, {_id: 4}, {_id: 6}, {_id: 3}, {_id: 8}, {_id: 7}, {_id: 9}]);
testSortAndSortWithLimit(
    {"a.c": 1, "a.b": -1, _id: 1},
    [{_id: 3}, {_id: 6}, {_id: 1}, {_id: 4}, {_id: 2}, {_id: 5}, {_id: 9}, {_id: 7}, {_id: 8}]);
testSortAndSortWithLimit(
    {"a.c": -1, "a.b": 1, _id: 1},
    [{_id: 7}, {_id: 6}, {_id: 4}, {_id: 9}, {_id: 5}, {_id: 8}, {_id: 1}, {_id: 2}, {_id: 3}]);
testSortAndSortWithLimit(
    {"a.c": -1, "a.b": -1, _id: 1},
    [{_id: 8}, {_id: 5}, {_id: 9}, {_id: 4}, {_id: 6}, {_id: 7}, {_id: 3}, {_id: 2}, {_id: 1}]);

// Basic tests for a sort pattern that contains a path of length 2 and a path of length 1, where
// one path is a prefix of the other path.
testSortAndSortWithLimit(
    {"a.c": 1, a: 1, _id: 1},
    [{_id: 2}, {_id: 5}, {_id: 1}, {_id: 4}, {_id: 6}, {_id: 3}, {_id: 8}, {_id: 7}, {_id: 9}]);
testSortAndSortWithLimit(
    {"a.c": 1, a: -1, _id: 1},
    [{_id: 3}, {_id: 6}, {_id: 1}, {_id: 4}, {_id: 2}, {_id: 5}, {_id: 9}, {_id: 7}, {_id: 8}]);
testSortAndSortWithLimit(
    {"a.c": -1, a: 1, _id: 1},
    [{_id: 7}, {_id: 6}, {_id: 4}, {_id: 9}, {_id: 5}, {_id: 8}, {_id: 1}, {_id: 2}, {_id: 3}]);
testSortAndSortWithLimit(
    {"a.c": -1, a: -1, _id: 1},
    [{_id: 8}, {_id: 5}, {_id: 9}, {_id: 4}, {_id: 6}, {_id: 7}, {_id: 3}, {_id: 2}, {_id: 1}]);

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

testSortAndSortWithLimit(
    {"a.c": 1, "a.b": 1, _id: 1},
    [{_id: 6}, {_id: 1}, {_id: 2}, {_id: 5}, {_id: 3}, {_id: 4}, {_id: 7}, {_id: 8}, {_id: 9}]);
testSortAndSortWithLimit(
    {"a.c": 1, "a.b": -1, _id: 1},
    [{_id: 6}, {_id: 3}, {_id: 1}, {_id: 4}, {_id: 5}, {_id: 2}, {_id: 9}, {_id: 8}, {_id: 7}]);
testSortAndSortWithLimit(
    {"a.c": -1, "a.b": 1, _id: 1},
    [{_id: 2}, {_id: 7}, {_id: 6}, {_id: 4}, {_id: 5}, {_id: 9}, {_id: 8}, {_id: 1}, {_id: 3}]);
testSortAndSortWithLimit(
    {"a.c": -1, "a.b": -1, _id: 1},
    [{_id: 2}, {_id: 8}, {_id: 5}, {_id: 9}, {_id: 4}, {_id: 6}, {_id: 7}, {_id: 3}, {_id: 1}]);

// More tests for a sort pattern that contains a path of length 2 and a path of length 1, where
// one path is a prefix of the other path.
testSortAndSortWithLimit(
    {"a.b": 1, a: 1, _id: 1},
    [{_id: 7}, {_id: 1}, {_id: 2}, {_id: 5}, {_id: 6}, {_id: 8}, {_id: 4}, {_id: 3}, {_id: 9}]);
testSortAndSortWithLimit(
    {"a.b": 1, a: -1, _id: 1},
    [{_id: 7}, {_id: 1}, {_id: 5}, {_id: 2}, {_id: 8}, {_id: 6}, {_id: 3}, {_id: 4}, {_id: 9}]);
testSortAndSortWithLimit(
    {"a.b": -1, a: 1, _id: 1},
    [{_id: 3}, {_id: 8}, {_id: 9}, {_id: 5}, {_id: 6}, {_id: 4}, {_id: 1}, {_id: 2}, {_id: 7}]);
testSortAndSortWithLimit(
    {"a.b": -1, a: -1, _id: 1},
    [{_id: 3}, {_id: 9}, {_id: 8}, {_id: 5}, {_id: 1}, {_id: 4}, {_id: 6}, {_id: 2}, {_id: 7}]);

// Tests for a sort pattern that contains two paths of length 2 that do not have a common prefix.
assert(coll.drop());
assert.commandWorked(coll.insert([
    {_id: 1, a: {b: 1}, c: [{d: [4, 1]}, {d: [1, 5]}]},
    {_id: 2, a: [{b: [1, 3]}, {b: [2, 4]}], c: {d: 2}},
    {_id: 3, a: {b: 1}, c: [{d: [6, 4]}, {d: [9, 7]}]},
    {_id: 4, a: [{b: [2]}, {b: [3, 2]}], c: {d: 4}},
    {_id: 5, a: {b: 1}, c: [{d: [2, 3]}, {d: [7, 6]}]},
    {_id: 6, a: [{b: []}, {b: 3}], c: {d: 4}},
    {_id: 7, a: {b: 2}, c: [{d: []}, {d: 2}]},
    {_id: 8, a: [{b: [2]}, {b: [3]}], c: {d: 3}},
    {_id: 9, a: {b: 3}, c: [{d: [8]}, {d: [6]}]},
]));

testSortAndSortWithLimit(
    {"a.b": 1, "c.d": 1, _id: 1},
    [{_id: 6}, {_id: 1}, {_id: 2}, {_id: 5}, {_id: 3}, {_id: 7}, {_id: 8}, {_id: 4}, {_id: 9}]);
testSortAndSortWithLimit(
    {"a.b": 1, "c.d": -1, _id: 1},
    [{_id: 6}, {_id: 3}, {_id: 5}, {_id: 1}, {_id: 2}, {_id: 4}, {_id: 8}, {_id: 7}, {_id: 9}]);
testSortAndSortWithLimit(
    {"a.b": -1, "c.d": 1, _id: 1},
    [{_id: 2}, {_id: 8}, {_id: 4}, {_id: 6}, {_id: 9}, {_id: 7}, {_id: 1}, {_id: 5}, {_id: 3}]);
testSortAndSortWithLimit(
    {"a.b": -1, "c.d": -1, _id: 1},
    [{_id: 2}, {_id: 9}, {_id: 4}, {_id: 6}, {_id: 8}, {_id: 7}, {_id: 3}, {_id: 5}, {_id: 1}]);

// Basic tests for a sort pattern that contains a path of length 3.
assert(coll.drop());
assert.commandWorked(coll.insert([
    {_id: 1, a: {b: {c: 2}}},
    {_id: 2, a: {b: {c: []}}},
    {_id: 3, a: [{b: []}, {b: {c: []}}]},
    {_id: 4, a: [{b: {c: 1}}]},
    {_id: 5, a: {b: [{c: 3}, {d: 1}]}},
    {_id: 6, a: {b: [{c: [5]}]}},
    {_id: 7, a: []},
    {_id: 8, a: {b: [{c: [1, 3]}, {c: [2, 5]}]}},
    {_id: 9, a: [{b: [{c: []}, {c: [1, 3]}]}]}
]));

testSortAndSortWithLimit(
    {"a.b.c": 1, _id: 1},
    [{_id: 2}, {_id: 3}, {_id: 9}, {_id: 5}, {_id: 7}, {_id: 4}, {_id: 8}, {_id: 1}, {_id: 6}]);
testSortAndSortWithLimit(
    {"a.b.c": 1, _id: -1},
    [{_id: 9}, {_id: 3}, {_id: 2}, {_id: 7}, {_id: 5}, {_id: 8}, {_id: 4}, {_id: 1}, {_id: 6}]);
testSortAndSortWithLimit(
    {"a.b.c": -1, _id: 1},
    [{_id: 6}, {_id: 8}, {_id: 5}, {_id: 9}, {_id: 1}, {_id: 4}, {_id: 3}, {_id: 7}, {_id: 2}]);
testSortAndSortWithLimit(
    {"a.b.c": -1, _id: -1},
    [{_id: 8}, {_id: 6}, {_id: 9}, {_id: 5}, {_id: 1}, {_id: 4}, {_id: 7}, {_id: 3}, {_id: 2}]);
})();
