// Basic integration tests for the $round and $trunc aggregation expressions.

(function() {
"use strict";

// For assertErrorCode.
load("jstests/aggregation/extras/utils.js");
load("jstests/libs/sbe_assert_error_override.js");  // Override error-code-checking APIs.

const coll = db.server19548;
coll.drop();

// Helper for testing that op returns expResult.
function testOp(exprName, value, expResult, place) {
    coll.drop();
    assert.commandWorked(coll.insert({a: value}));
    const project = place === undefined ? {[exprName]: "$a"} : {[exprName]: ["$a", place]};
    const pipeline = [{$project: {_id: 0, result: project}}];
    assert.eq(coll.aggregate(pipeline).toArray(), [{result: expResult}]);
}

function testRound(value, expResult, place) {
    testOp("$round", value, expResult, place);
}

function testTrunc(value, expResult, place) {
    testOp("$trunc", value, expResult, place);
}

// Test $trunc and $round with one argument.
testTrunc(NumberLong(4), NumberLong(4));
testTrunc(NumberLong(4), NumberLong(4));
testTrunc(NaN, NaN);
testTrunc(Infinity, Infinity);
testTrunc(-Infinity, -Infinity);
testTrunc(null, null);
testTrunc(-2.0, -2.0);
testTrunc(0.9, 0.0);
testTrunc(-1.2, -1.0);
testTrunc(NumberDecimal("-1.6"), NumberDecimal("-1"));

testRound(NumberLong(4), NumberLong(4));
testRound(NaN, NaN);
testRound(Infinity, Infinity);
testRound(-Infinity, -Infinity);
testRound(null, null);
testRound(-2.0, -2.0);
testRound(0.9, 1.0);
testRound(-1.2, -1.0);
testRound(NumberDecimal("-1.6"), NumberDecimal("-2"));

// Test $trunc and $round with two arguments.
testTrunc(1.298, 1, 0);
testTrunc(1.298, 1.2, 1);
testTrunc(23.298, 20, -1);
testTrunc(NumberDecimal("1.298"), NumberDecimal("1"), 0);
testTrunc(NumberDecimal("1.298"), NumberDecimal("1.2"), 1);
testTrunc(NumberDecimal("23.298"), NumberDecimal("2E+1"), -1);
testTrunc(1.298, 1.298, 100);
testTrunc(NumberDecimal("1.298912343250054252245154325"),
          NumberDecimal("1.29891234325005425224"),
          NumberLong("20"));
testTrunc(NumberDecimal("1.298"),
          NumberDecimal("1.298000000000000000000000000000000"),
          NumberDecimal("100"));

testRound(1.298, 1, 0);
testRound(1.298, 1.3, 1);
testRound(23.298, 20, -1);
testRound(NumberDecimal("1.298"), NumberDecimal("1"), 0);
testRound(NumberDecimal("1.298"), NumberDecimal("1.3"), 1);
testRound(NumberDecimal("23.298"), NumberDecimal("2E+1"), -1);
testRound(1.298, 1.298, 100);
testRound(NumberDecimal("1.298912343250054252245154325"),
          NumberDecimal("1.29891234325005425225"),
          NumberLong("20"));
testRound(NumberDecimal("1.298"),
          NumberDecimal("1.298000000000000000000000000000000"),
          NumberDecimal("100"));

// Test $round overflow.
testRound(NumberInt("2147483647"), NumberLong("2147483650"), -1);
assertErrorCode(coll, [{$project: {a: {$round: [NumberLong("9223372036854775806"), -1]}}}], 51080);

// Test $trunc and $round with more than 2 arguments.
assertErrorCode(coll, [{$project: {a: {$trunc: [1, 2, 3]}}}], 28667);
assertErrorCode(coll, [{$project: {a: {$round: [1, 2, 3]}}}], 28667);

// Test non-numeric input to $trunc and $round.
assertErrorCode(coll, [{$project: {a: {$round: "string"}}}], 51081);
assertErrorCode(coll, [{$project: {a: {$trunc: "string"}}}], 51081);

// Test NaN and Infinity numeric args.
testRound(Infinity, Infinity, 0);
testRound(-Infinity, -Infinity, 0);
testRound(NaN, NaN, 0);
testRound(NumberDecimal("Infinity"), NumberDecimal("Infinity"), 0);
testRound(NumberDecimal("-Infinity"), NumberDecimal("-Infinity"), 0);
testRound(NumberDecimal("NaN"), NumberDecimal("NaN"), 0);
testRound(null, null, 1);
testRound(1, null, null);

testTrunc(Infinity, Infinity, 0);
testTrunc(-Infinity, -Infinity, 0);
testTrunc(NaN, NaN, 0);
testTrunc(NumberDecimal("Infinity"), NumberDecimal("Infinity"), 0);
testTrunc(NumberDecimal("-Infinity"), NumberDecimal("-Infinity"), 0);
testTrunc(NumberDecimal("NaN"), NumberDecimal("NaN"), 0);

// Test precision arguments that are out of bounds.
assert.commandWorked(coll.insert({}));
assertErrorCode(coll, [{$project: {a: {$round: [1, NumberLong("101")]}}}], 51083);
assertErrorCode(coll, [{$project: {a: {$round: [1, NumberLong("-21")]}}}], 51083);
assertErrorCode(coll, [{$project: {a: {$round: [1, NumberDecimal("101")]}}}], 51083);
assertErrorCode(coll, [{$project: {a: {$round: [1, NumberDecimal("-21")]}}}], 51083);
assertErrorCode(coll, [{$project: {a: {$round: [1, NumberInt("101")]}}}], 51083);
assertErrorCode(coll, [{$project: {a: {$round: [1, NumberInt("-21")]}}}], 51083);
assertErrorCode(coll, [{$project: {a: {$round: [1, 101]}}}], 51083);
assertErrorCode(coll, [{$project: {a: {$round: [1, -21]}}}], 51083);
assertErrorCode(coll, [{$project: {a: {$trunc: [1, NumberLong("101")]}}}], 51083);
assertErrorCode(coll, [{$project: {a: {$trunc: [1, NumberLong("-21")]}}}], 51083);
assertErrorCode(coll, [{$project: {a: {$trunc: [1, NumberDecimal("101")]}}}], 51083);
assertErrorCode(coll, [{$project: {a: {$trunc: [1, NumberDecimal("-21")]}}}], 51083);
assertErrorCode(coll, [{$project: {a: {$trunc: [1, NumberInt("101")]}}}], 51083);
assertErrorCode(coll, [{$project: {a: {$trunc: [1, NumberInt("-21")]}}}], 51083);
assertErrorCode(coll, [{$project: {a: {$trunc: [1, 101]}}}], 51083);
assertErrorCode(coll, [{$project: {a: {$trunc: [1, -21]}}}], 51083);

// Test non-integral precision arguments.
assertErrorCode(coll, [{$project: {a: {$round: [1, NumberDecimal("1.4")]}}}], 51082);
assertErrorCode(coll, [{$project: {a: {$trunc: [1, 10.5]}}}], 51082);
assertErrorCode(coll, [{$project: {a: {$round: [0, NaN]}}}], 31109);
assertErrorCode(coll, [{$project: {a: {$round: [0, NumberDecimal("NaN")]}}}], 51082);
assertErrorCode(coll, [{$project: {a: {$round: [BinData(0, ""), 0]}}}], 51081);
assertErrorCode(coll, [{$project: {a: {$round: [0, BinData(0, "")]}}}], 16004);
assertErrorCode(coll, [{$project: {a: {$round: MinKey}}}], 51081);
assertErrorCode(coll, [{$project: {a: {$round: MaxKey}}}], 51081);
}());
