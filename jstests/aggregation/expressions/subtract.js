// Tests for $subtract aggregation expression
(function() {
"use strict";

const coll = db.subtract_coll;
coll.drop();

assert.commandWorked(coll.insert({_id: 0, lhs: 1, rhs: 1}));
assert.commandWorked(coll.insert({_id: 1, lhs: -2000000000, rhs: 2000000000}));
assert.commandWorked(
    coll.insert({_id: 2, lhs: NumberLong(-20000000000), rhs: NumberLong(20000000000)}));
assert.commandWorked(coll.insert({_id: 3, lhs: 10.5, rhs: 0.5}));
assert.commandWorked(
    coll.insert({_id: 4, lhs: NumberDecimal("10000.12345"), rhs: NumberDecimal("10.1234")}));
assert.commandWorked(coll.insert({_id: 5, lhs: new Date(1912392670000), rhs: 70000}));
assert.commandWorked(
    coll.insert({_id: 6, lhs: new Date(1912392670000), rhs: new Date(1912392600000)}));

const result =
    coll.aggregate([{$project: {diff: {$subtract: ["$lhs", "$rhs"]}}}, {$sort: {_id: 1}}])
        .toArray();
assert.eq(result[0].diff, 0);
assert.eq(result[1].diff, NumberLong("-4000000000"));
assert.eq(result[2].diff, NumberLong("-40000000000"));
assert.eq(result[3].diff, 10.0);
assert.eq(result[4].diff, NumberDecimal("9990.00005"));
assert.eq(result[5].diff, new Date(1912392600000));
assert.eq(result[6].diff, 70000);
}());
