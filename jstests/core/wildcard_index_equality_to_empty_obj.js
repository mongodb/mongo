/**
 * Tests that a $** index can support queries which test for equality to empty nested objects.
 */
(function() {
"use strict";

const coll = db.wildcard_index_equality_to_empty_obj;
coll.drop();

assert.commandWorked(coll.insert([
    {_id: 0},
    {_id: 1, a: null},
    {_id: 2, a: []},
    {_id: 3, a: {}},
    {_id: 4, a: [{}]},
    {_id: 5, a: [[{}]]},
    {_id: 6, a: [1, 2, {}]},
    {_id: 7, a: {b: 1}},
    {_id: 8, a: 3},
    {_id: 9, a: {b: {}}},
    {_id: 10, a: [0, {b: {}}]},
]));

assert.commandWorked(coll.createIndex({"$**": 1}));

// Test that a comparison to empty object query returns the expected results when the $** index
// is hinted.
let results = coll.find({a: {}}, {_id: 1}).sort({_id: 1}).hint({"$**": 1}).toArray();
assert.eq(results, [{_id: 3}, {_id: 4}, {_id: 6}]);

// Result set should be the same as when hinting a COLLSCAN and with no hint.
assert.eq(results, coll.find({a: {}}, {_id: 1}).sort({_id: 1}).hint({$natural: 1}).toArray());
assert.eq(results, coll.find({a: {}}, {_id: 1}).sort({_id: 1}).toArray());

// Repeat the above query, but express it using $lte:{}, which is a synonym for $eq:{}.
results = coll.find({a: {$lte: {}}}, {_id: 1}).sort({_id: 1}).hint({"$**": 1}).toArray();
assert.eq(results, [{_id: 3}, {_id: 4}, {_id: 6}]);
assert.eq(results,
          coll.find({a: {$lte: {}}}, {_id: 1}).sort({_id: 1}).hint({$natural: 1}).toArray());
assert.eq(results, coll.find({a: {$lte: {}}}, {_id: 1}).sort({_id: 1}).toArray());

// Test that an inequality to empty object query results in an error when the $** index is
// hinted.
assert.throws(() => coll.find({a: {$gte: {}}}, {_id: 1}).sort({_id: 1}).hint({"$**": 1}).toArray());

// Test that an inequality to empty object query returns the expected results in the presence of
// the $** index.
results = coll.find({a: {$gte: {}}}, {_id: 1}).sort({_id: 1}).toArray();
assert.eq(results, [{_id: 3}, {_id: 4}, {_id: 6}, {_id: 7}, {_id: 9}, {_id: 10}]);

// Result set should be the same as when hinting a COLLSCAN and with no hint.
assert.eq(results,
          coll.find({a: {$gte: {}}}, {_id: 1}).sort({_id: 1}).hint({$natural: 1}).toArray());
assert.eq(results, coll.find({a: {$gte: {}}}, {_id: 1}).sort({_id: 1}).toArray());

// Test that an $in with an empty object returns the expected results when the $** index is
// hinted.
results = coll.find({a: {$in: [3, {}]}}, {_id: 1}).sort({_id: 1}).hint({"$**": 1}).toArray();
assert.eq(results, [{_id: 3}, {_id: 4}, {_id: 6}, {_id: 8}]);

// Result set should be the same as when hinting a COLLSCAN and with no hint.
assert.eq(results,
          coll.find({a: {$in: [3, {}]}}, {_id: 1}).sort({_id: 1}).hint({$natural: 1}).toArray());
assert.eq(results, coll.find({a: {$in: [3, {}]}}, {_id: 1}).sort({_id: 1}).toArray());

// Test that a wildcard index can support equality to an empty object on a dotted field.
results = coll.find({"a.b": {$eq: {}}}, {_id: 1}).sort({_id: 1}).hint({"$**": 1}).toArray();
assert.eq(results, [{_id: 9}, {_id: 10}]);

// Result set should be the same as when hinting a COLLSCAN and with no hint.
assert.eq(results,
          coll.find({"a.b": {$eq: {}}}, {_id: 1}).sort({_id: 1}).hint({$natural: 1}).toArray());
assert.eq(results, coll.find({"a.b": {$eq: {}}}, {_id: 1}).sort({_id: 1}).toArray());
}());
