// Tests the behavior of $mod for match expressions.
// @tags: [
//   assumes_balancer_off,
//   # Uses $where operator
//   requires_scripting,
//   sbe_incompatible,
// ]

(function() {
"use strict";

const coll = db.mod_with_where;
coll.drop();

assert.commandWorked(coll.insert([{a: 1}, {a: 2}, {a: 11}, {a: 20}, {a: "asd"}, {a: "adasdas"}]));

// Check basic mod usage.
assert.eq(2, coll.find("this.a % 10 == 1").itcount(), "A1");
assert.eq(2, coll.find({a: {$mod: [10, 1]}}).itcount(), "A2");
assert.eq(
    0,
    coll.find({a: {$mod: [10, 1]}}).explain("executionStats").executionStats.totalKeysExamined,
    "A3");

assert.commandWorked(coll.createIndex({a: 1}));

// Check mod with an index.
assert.eq(2, coll.find("this.a % 10 == 1").itcount(), "B1");
assert.eq(2, coll.find({a: {$mod: [10, 1]}}).itcount(), "B2");
assert.eq(1, coll.find("this.a % 10 == 0").itcount(), "B3");
assert.eq(1, coll.find({a: {$mod: [10, 0]}}).itcount(), "B4");
assert.eq(
    4,
    coll.find({a: {$mod: [10, 1]}}).explain("executionStats").executionStats.totalKeysExamined,
    "B5");
assert.eq(1, coll.find({a: {$gt: 5, $mod: [10, 1]}}).itcount());

assert(coll.drop());
}());
