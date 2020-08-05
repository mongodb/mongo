// Tests the behavior of $size for match expressions.
// @tags: [
//   sbe_incompatible,
// ]

(function() {
"use strict";

const coll = db.jstests_find_size;
coll.drop();

// Check nested arrays.
assert.commandWorked(coll.insert([
    {a: [{b: [1, 2, 3]}, {b: 1}]},
    {a: [1, [2], [3]]},
    {a: [1, [2]]},
    {a: [[1, [1]]]},
    {a: [[[1]]]},
    {a: 1},
    {a: {}},
]));

assert.eq(1, coll.find({"a": {$size: 3}}).itcount());
assert.eq(2, coll.find({"a": {$size: 2}}).itcount());
assert.eq(2, coll.find({"a": {$size: 1}}).itcount());
assert.eq(0, coll.find({"a": {$size: 4}}).itcount());
assert.eq(0, coll.find({"b": {$size: 1}}).itcount());

// Check dotted paths.
assert.commandWorked(coll.insert([{a: {b: [1, 2]}}, {a: {b: {c: [1]}}}]));

assert.eq(1, coll.find({"a.b": {$size: 2}}).itcount());
assert.eq(1, coll.find({"a.b.c": {$size: 1}}).itcount());
assert.eq(0, coll.find({"a.b.c.d": {$size: 1}}).itcount());
assert.eq(0, coll.find({"a.b": {$size: 1}}).itcount());

// Check nested arrays with dotted paths.
assert.commandWorked(coll.insert({a: {b: {c: [1, [2]]}}, b: {a: {c: []}, b: {a: [[1, 2, 3]]}}}));

assert.eq(1, coll.find({"a.b.c": {$size: 2}}).itcount());
assert.eq(1, coll.find({"b.a.c": {$size: 0}}).itcount());
assert.eq(1, coll.find({"b.b.a": {$size: 1}}).itcount());
assert.eq(0, coll.find({"b.b.a": {$size: 0}}).itcount());
assert.eq(0, coll.find({"a.b.b.a": {$size: 1}}).itcount());

assert(coll.drop());

// Check more nested arrays with dotted paths and that as long as one
// of the elements of array matches, the document matches.
assert.commandWorked(coll.insert([
    {a: [{b: [1, 2, 3]}, {b: 1}]},
    {a: [{b: [1, [2]]}, {b: [1]}]},
    {a: [{b: [{c: [1]}, {c: []}]}, {b: [1]}]},
    {a: {b: [1, [2], [[3]]]}},
    {a: {b: [1, 2, 3]}},
    {b: {a: [1, [2], [[3]]]}},
    {b: {a: []}},
]));

assert.eq(3, coll.find({"a.b": {$size: 3}}).itcount());
assert.eq(2, coll.find({"a.b": {$size: 2}}).itcount());
assert.eq(1, coll.find({"a.b.c": {$size: 1}}).itcount());
assert.eq(1, coll.find({"a.b.c": {$size: 0}}).itcount());
assert.eq(1, coll.find({"b.a": {$size: 3}}).itcount());
assert.eq(1, coll.find({"b.a": {$size: 0}}).itcount());
assert.eq(0, coll.find({"a.b.c": {$size: 3}}).itcount());
assert.eq(0, coll.find({"a.b.c.d": {$size: 1}}).itcount());

// Check arrays of size 0.
assert.commandWorked(coll.insert([{a: []}, {a: []}, {a: {b: []}}]));

assert.eq(2, coll.find({"a": {$size: 0}}).itcount());
assert.eq(1, coll.find({"a.b": {$size: 0}}).itcount());

assert(coll.drop());

// Check ints and longs.
assert.commandWorked(coll.insert([{a: []}, {a: []}, {a: [1]}, {a: [1, 2, 3, 4]}]));

assert.eq(2, coll.find({"a": {$size: 0}}).itcount());
assert.eq(2, coll.find({"a": {$size: NumberLong(0)}}).itcount());
assert.eq(1, coll.find({"a": {$size: NumberInt(4)}}).itcount());

// Check bad inputs.
const badInputs = [-1, NumberLong(-10000), "str", 3.2, 0.1, NumberLong(-9223372036854775808)];
badInputs.forEach(function(input) {
    assert.commandFailed(db.runCommand({find: coll.getName(), filter: {"a": {$size: input}}}),
                         "$size argument " + input + " should have failed");
});
}());
