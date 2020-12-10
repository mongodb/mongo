// Confirm correctness of $mod evaluation in find projection.

(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");  // For assertArrayEq.

// Note that the "getParameter" command is expected to fail in versions of mongod that do not yet
// include the slot-based execution engine. When that happens, however, 'isSBEEnabled' still
// correctly evaluates to false.
const isSBEEnabled = (() => {
    const getParam = db.adminCommand({getParameter: 1, featureFlagSBE: 1});
    return getParam.hasOwnProperty("featureFlagSBE") && getParam.featureFlagSBE.value;
})();
if (isSBEEnabled) {
    // Override error-code-checking APIs. We only load this when SBE is explicitly enabled, because
    // it causes failures in the parallel suites.
    load("jstests/libs/sbe_assert_error_override.js");
}

const coll = db.projection_expr_mod;
coll.drop();

assert.commandWorked(coll.insertMany([
    {n: "double % double", v: 138.5, m: 3.0},
    {n: "double % long", v: 138.5, m: NumberLong(3)},
    {n: "double % int", v: 138.5, m: NumberInt(3)},
    {n: "int % double", v: NumberInt(8), m: 3.25},
    {n: "int % double int", v: NumberInt(8), m: 3.0},
    {n: "int % int", v: NumberInt(8), m: NumberInt(3)},
    {n: "int % long", v: NumberInt(8), m: NumberLong(3)},
    {n: "long % double", v: NumberLong(8), m: 3.25},
    {n: "long % double int", v: NumberLong(8), m: 3.0},
    {n: "long % double long", v: NumberLong(500000000000), m: 450000000000.0},
    {n: "long % int", v: NumberLong(8), m: NumberInt(3)},
    {n: "long % long", v: NumberLong(8), m: NumberLong(3)},
    {n: "very long % very long", v: NumberLong(800000000000), m: NumberLong(300000000000)}
]));

const result = coll.find({}, {f: {$mod: ["$v", "$m"]}, _id: 0, n: 1}).toArray();
const expectedResult = [
    {n: "double % double", f: 0.5},
    {n: "double % long", f: 0.5},
    {n: "double % int", f: 0.5},
    {n: "int % double", f: 1.5},
    {n: "int % double int", f: 2},
    {n: "int % int", f: 2},
    {n: "int % long", f: NumberLong(2)},
    {n: "long % double", f: 1.5},
    {n: "long % double int", f: NumberLong(2)},
    {n: "long % double long", f: 50000000000},
    {n: "long % int", f: NumberLong(2)},
    {n: "long % long", f: NumberLong(2)},
    {n: "very long % very long", f: NumberLong(200000000000)}
];
assertArrayEq({actual: result, expected: expectedResult});

//
// Confirm error cases.
//

// Confirm mod by 0 fails in an expected manner.
assert(coll.drop());
assert.commandWorked(coll.insert({a: 10}));
let error = assert.throws(() => coll.find({}, {f: {$mod: ["$a", 0]}, _id: 0, n: 1}).toArray());
assert.commandFailedWithCode(error, 16610);

assert(coll.drop());
assert.commandWorked(coll.insert({a: NumberInt(10)}));
error =
    assert.throws(() => coll.find({}, {f: {$mod: ["$a", NumberInt(0)]}, _id: 0, n: 1}).toArray());
assert.commandFailedWithCode(error, 16610);

assert(coll.drop());
assert.commandWorked(coll.insert({a: NumberLong(10)}));
error =
    assert.throws(() => coll.find({}, {f: {$mod: ["$a", NumberLong(0)]}, _id: 0, n: 1}).toArray());
assert.commandFailedWithCode(error, 16610);

// Clear collection again and reset.
assert(coll.drop());
assert.commandWorked(coll.insert({a: 10}));

// Confirm expected behavior for NaN and Infinity values.
assert.eq(coll.findOne({}, {f: {$mod: ["$a", NaN]}, _id: 0}), {f: NaN});
assert.eq(coll.findOne({}, {f: {$mod: ["$a", Infinity]}, _id: 0}), {f: 10});
assert.eq(coll.findOne({}, {f: {$mod: ["$a", -Infinity]}, _id: 0}), {f: 10});
assert.eq(coll.findOne({}, {f: {$mod: [Infinity, "$a"]}, _id: 0}), {f: NaN});
assert.eq(coll.findOne({}, {f: {$mod: [-Infinity, "$a"]}, _id: 0}), {f: NaN});
assert.eq(coll.findOne({}, {f: {$mod: [NaN, "$a"]}, _id: 0}), {f: NaN});
})();
