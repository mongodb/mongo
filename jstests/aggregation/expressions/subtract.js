load("jstests/aggregation/extras/utils.js");        // For assertErrorCode and assertErrMsgContains.
load("jstests/libs/sbe_assert_error_override.js");  // Override error-code-checking APIs.

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
// Doubles are rounded to int64 when subtracted from Date
assert.commandWorked(coll.insert({_id: 7, lhs: new Date(1683794065002), rhs: 0.5}));
assert.commandWorked(coll.insert({_id: 8, lhs: new Date(1683794065002), rhs: 1.4}));
assert.commandWorked(coll.insert({_id: 9, lhs: new Date(1683794065002), rhs: 1.5}));
assert.commandWorked(coll.insert({_id: 10, lhs: new Date(1683794065002), rhs: 1.7}));
// Decimals are rounded to int64, when tie rounded to even, when subtracted from Date
assert.commandWorked(
    coll.insert({_id: 11, lhs: new Date(1683794065002), rhs: new NumberDecimal("1.4")}));
assert.commandWorked(
    coll.insert({_id: 12, lhs: new Date(1683794065002), rhs: new NumberDecimal("1.5")}));
assert.commandWorked(
    coll.insert({_id: 13, lhs: new Date(1683794065002), rhs: new NumberDecimal("1.7")}));
assert.commandWorked(
    coll.insert({_id: 14, lhs: new Date(1683794065002), rhs: new NumberDecimal("2.5")}));

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
assert.eq(result[7].diff, new Date(1683794065001));
assert.eq(result[8].diff, new Date(1683794065001));
assert.eq(result[9].diff, new Date(1683794065000));
assert.eq(result[10].diff, new Date(1683794065000));
assert.eq(result[11].diff, new Date(1683794065001));
assert.eq(result[12].diff, new Date(1683794065000));
assert.eq(result[13].diff, new Date(1683794065000));
assert.eq(result[14].diff, new Date(1683794065000));

// Following cases will report overflow error
coll.drop();

assert.commandWorked(coll.insert([{
    _id: 0,
    veryBigNegativeLong: NumberLong("-9223372036854775808"),
    veryBigNegativeDouble: -9223372036854775808,
    veryBigNegativeDecimal: NumberDecimal("-9223372036854775808")
}]));

let pipeline = [{$project: {res: {$subtract: [new Date(10), "$veryBigNegativeLong"]}}}];
assertErrCodeAndErrMsgContains(coll, pipeline, ErrorCodes.Overflow, "date overflow");

pipeline = [{$project: {res: {$subtract: [new Date(10), "$veryBigNegativeDouble"]}}}];
assertErrCodeAndErrMsgContains(coll, pipeline, ErrorCodes.Overflow, "date overflow");

pipeline = [{$project: {res: {$subtract: [new Date(10), "$veryBigNegativeDecimal"]}}}];
assertErrCodeAndErrMsgContains(coll, pipeline, ErrorCodes.Overflow, "date overflow");
}());
