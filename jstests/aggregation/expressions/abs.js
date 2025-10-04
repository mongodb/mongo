// SERVER-10176: Add $abs aggregation expression.

import "jstests/libs/query/sbe_assert_error_override.js";

import {assertErrorCode} from "jstests/aggregation/extras/utils.js";

let coll = db.abs_expr;
coll.drop();

// Valid types (numeric and null):
assert.commandWorked(coll.insert({_id: 0, a: 5}));
assert.commandWorked(coll.insert({_id: 1, a: -5}));
assert.commandWorked(coll.insert({_id: 2, a: 5.5}));
assert.commandWorked(coll.insert({_id: 3, a: -5.5}));
assert.commandWorked(coll.insert({_id: 4, a: NumberInt("5")}));
assert.commandWorked(coll.insert({_id: 5, a: NumberInt("-5")}));
assert.commandWorked(coll.insert({_id: 6, a: NumberLong("5")}));
assert.commandWorked(coll.insert({_id: 7, a: NumberLong("-5")}));
assert.commandWorked(coll.insert({_id: 8, a: 0.0}));
assert.commandWorked(coll.insert({_id: 9, a: -0.0}));
assert.commandWorked(coll.insert({_id: 10, a: NumberInt("0")}));
// INT_MIN is -(2 ^ 31).
assert.commandWorked(coll.insert({_id: 11, a: NumberInt(-Math.pow(2, 31))}));
assert.commandWorked(coll.insert({_id: 12, a: -Math.pow(2, 31)}));
// 1152921504606846977 is 2^60 + 1, an integer that can't be represented precisely as a double.
assert.commandWorked(coll.insert({_id: 13, a: NumberLong("1152921504606846977")}));
assert.commandWorked(coll.insert({_id: 14, a: NumberLong("-1152921504606846977")}));
assert.commandWorked(coll.insert({_id: 15, a: null}));
assert.commandWorked(coll.insert({_id: 16, a: undefined}));
assert.commandWorked(coll.insert({_id: 17, a: NaN}));
assert.commandWorked(coll.insert({_id: 18}));

let results = coll.aggregate([{$project: {a: {$abs: "$a"}}}, {$sort: {_id: 1}}]).toArray();
assert.eq(results, [
    {_id: 0, a: 5},
    {_id: 1, a: 5},
    {_id: 2, a: 5.5},
    {_id: 3, a: 5.5},
    {_id: 4, a: 5},
    {_id: 5, a: 5},
    {_id: 6, a: NumberLong("5")},
    {_id: 7, a: NumberLong("5")},
    {_id: 8, a: 0},
    {_id: 9, a: 0},
    {_id: 10, a: 0},
    {_id: 11, a: NumberLong(Math.pow(2, 31))},
    {_id: 12, a: Math.pow(2, 31)},
    {_id: 13, a: NumberLong("1152921504606846977")},
    {_id: 14, a: NumberLong("1152921504606846977")},
    {_id: 15, a: null},
    {_id: 16, a: null},
    {_id: 17, a: NaN},
    {_id: 18, a: null},
]);

// Using $abs on a string literal expression.
assertErrorCode(coll, [{$project: {a: {$abs: "string"}}}], 28765);

// Using $abs on LLONG_MIN (-2 ^ 63) as a literal expression.
assertErrorCode(coll, [{$project: {a: {$abs: NumberLong("-9223372036854775808")}}}], 28680);

// Using $abs on a string value.
assert(coll.drop());
assert.commandWorked(coll.insert({_id: 0, a: "string"}));
assertErrorCode(coll, [{$project: {a: {$abs: "$a"}}}], 28765);

// Using $abs on LLONG_MIN (-2 ^ 63) as a value.
assert(coll.drop());
assert.commandWorked(coll.insert({_id: 0, a: NumberLong("-9223372036854775808")}));
assertErrorCode(coll, [{$project: {a: {$abs: "$a"}}}], 28680);
