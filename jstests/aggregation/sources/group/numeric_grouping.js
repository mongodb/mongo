/**
 * Tests that numbers that are equivalent but have different types are grouped together.
 */
(function() {
"use strict";
const coll = db.numeric_grouping;

coll.drop();

assert.commandWorked(coll.insert({key: new NumberInt(24), value: 17}));
assert.commandWorked(coll.insert({key: new NumberLong(24), value: 8}));
assert.commandWorked(coll.insert({key: 24, value: 5}));

assert.commandWorked(coll.insert({key: new NumberInt(42), value: 11}));
assert.commandWorked(coll.insert({key: new NumberLong(42), value: 13}));
assert.commandWorked(coll.insert({key: 42, value: 6}));

const results = coll.aggregate({$group: {_id: "$key", s: {$sum: "$value"}}}).toArray();

assert.eq(results.length, 2, tojson(results));

// Both groups should sum to 30.
assert.eq(results[0].s, 30, tojson(results));
assert.eq(results[1].s, 30, tojson(results));
}());
