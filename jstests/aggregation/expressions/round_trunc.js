// Basic integration tests for the $round and $trunc aggregation expressions.

(function() {
"use strict";

// For assertErrorCode.
load("jstests/aggregation/extras/utils.js");

var coll = db.server19548;
coll.drop();
// Seed collection so that the pipeline will execute.
assert.writeOK(coll.insert({}));

// Helper for testing that op returns expResult.
function testOp(op, expResult) {
    var pipeline = [{$project: {_id: 0, result: op}}];
    assert.eq(coll.aggregate(pipeline).toArray(), [{result: expResult}]);
}

// Test $trunc and $round with one argument.
testOp({$trunc: NumberLong(4)}, NumberLong(4));
testOp({$trunc: NaN}, NaN);
testOp({$trunc: Infinity}, Infinity);
testOp({$trunc: -Infinity}, -Infinity);
testOp({$trunc: null}, null);
testOp({$trunc: -2.0}, -2.0);
testOp({$trunc: 0.9}, 0.0);
testOp({$trunc: -1.2}, -1.0);
testOp({$trunc: NumberDecimal("-1.6")}, NumberDecimal("-1"));

testOp({$round: NumberLong(4)}, NumberLong(4));
testOp({$round: NaN}, NaN);
testOp({$round: Infinity}, Infinity);
testOp({$round: -Infinity}, -Infinity);
testOp({$round: null}, null);
testOp({$round: -2.0}, -2.0);
testOp({$round: 0.9}, 1.0);
testOp({$round: -1.2}, -1.0);
testOp({$round: NumberDecimal("-1.6")}, NumberDecimal("-2"));

// Test $trunc and $round with two arguments.
testOp({$trunc: [1.298, 0]}, 1);
testOp({$trunc: [1.298, 1]}, 1.2);
testOp({$trunc: [23.298, -1]}, 20);
testOp({$trunc: [NumberDecimal("1.298"), 0]}, NumberDecimal("1"));
testOp({$trunc: [NumberDecimal("1.298"), 1]}, NumberDecimal("1.2"));
testOp({$trunc: [NumberDecimal("23.298"), -1]}, NumberDecimal("2E+1"));
testOp({$trunc: [1.298, 100]}, 1.298);
testOp({$trunc: [NumberDecimal("1.298912343250054252245154325"), NumberLong("20")]},
       NumberDecimal("1.29891234325005425224"));
testOp({$trunc: [NumberDecimal("1.298"), NumberDecimal("100")]},
       NumberDecimal("1.298000000000000000000000000000000"));

testOp({$round: [1.298, 0]}, 1);
testOp({$round: [1.298, 1]}, 1.3);
testOp({$round: [23.298, -1]}, 20);
testOp({$round: [NumberDecimal("1.298"), 0]}, NumberDecimal("1"));
testOp({$round: [NumberDecimal("1.298"), 1]}, NumberDecimal("1.3"));
testOp({$round: [NumberDecimal("23.298"), -1]}, NumberDecimal("2E+1"));
testOp({$round: [1.298, 100]}, 1.298);
testOp({$round: [NumberDecimal("1.298912343250054252245154325"), NumberLong("20")]},
       NumberDecimal("1.29891234325005425225"));
testOp({$round: [NumberDecimal("1.298"), NumberDecimal("100")]},
       NumberDecimal("1.298000000000000000000000000000000"));

// Test $round overflow.
testOp({$round: [NumberInt("2147483647"), -1]}, NumberLong("2147483650"));
assertErrorCode(coll, [{$project: {a: {$round: [NumberLong("9223372036854775806"), -1]}}}], 51080);

// Test $trunc and $round with more than 2 arguments.
assertErrorCode(coll, [{$project: {a: {$trunc: [1, 2, 3]}}}], 28667);
assertErrorCode(coll, [{$project: {a: {$round: [1, 2, 3]}}}], 28667);

// Test non-numeric input to $trunc and $round.
assertErrorCode(coll, [{$project: {a: {$round: "string"}}}], 51081);
assertErrorCode(coll, [{$project: {a: {$trunc: "string"}}}], 51081);

// Test NaN and Infinity numeric args.
testOp({$round: [Infinity, 0]}, Infinity);
testOp({$round: [-Infinity, 0]}, -Infinity);
testOp({$round: [NaN, 0]}, NaN);
testOp({$round: [NumberDecimal("Infinity"), 0]}, NumberDecimal("Infinity"));
testOp({$round: [NumberDecimal("-Infinity"), 0]}, NumberDecimal("-Infinity"));
testOp({$round: [NumberDecimal("NaN"), 0]}, NumberDecimal("NaN"));

testOp({$trunc: [Infinity, 0]}, Infinity);
testOp({$trunc: [-Infinity, 0]}, -Infinity);
testOp({$trunc: [NaN, 0]}, NaN);
testOp({$trunc: [NumberDecimal("Infinity"), 0]}, NumberDecimal("Infinity"));
testOp({$trunc: [NumberDecimal("-Infinity"), 0]}, NumberDecimal("-Infinity"));
testOp({$trunc: [NumberDecimal("NaN"), 0]}, NumberDecimal("NaN"));

// Test precision arguments that are out of bounds.
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
}());
