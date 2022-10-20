// SERVER-32930: Basic integration tests for trigonometric aggregation expressions.

(function() {
"use strict";
// For assertErrorCode.
load("jstests/aggregation/extras/utils.js");
load('jstests/libs/sbe_assert_error_override.js');  // Override error-code-checking APIs.

const coll = db.expression_trigonometric;
coll.drop();

// Constructs and inserts a document containing 'val', which are the arguments to trigonometric
// aggregation expression 'op'. If an aggregation expression has constant arguments, it will be
// constant folded and will not be evaluated during query execution. Embedding in a document ensures
// we evaluate expressions during query execution. eg. {$acos: NumberInt(1)} -> {$acos: "$y"} on
// document {"y" : NumberInt(1)}
function convertToOpOnDocument(op, val) {
    let nonOptimizedOp = {};
    if (Array.isArray(val) && val.length === 2) {
        nonOptimizedOp = {[op]: ["$y", "$x"]};
        assert.commandWorked(coll.insert({"y": val[0], "x": val[1]}));
    } else {
        nonOptimizedOp = {[op]: "$y"};
        assert.commandWorked(coll.insert({"y": val}));
    }
    return nonOptimizedOp;
}

function testOp(op, val, expResult) {
    const nonOptimizedOp = convertToOpOnDocument(op, val);
    const pipeline = [{$project: {_id: 0, result: nonOptimizedOp}}];
    assert.eq(coll.aggregate(pipeline).toArray(), [{result: expResult}]);
    assert(coll.drop());
}

// Helper for testing that the aggregation expression 'op' returns expResult, approximately,
// since NumberDecimal has so many representations for a given number (0 versus 0e-40 for
// instance).
function testOpApprox(op, val, expResult) {
    const nonOptimizedOp = convertToOpOnDocument(op, val);
    const pipeline = [{$project: {_id: 0, result: nonOptimizedOp}}];
    const res = coll.aggregate(pipeline).toArray();
    const {result} = res[0];
    const pipeline2 = {
        $project: {
            difference: {$abs: {$convert: {input: {$subtract: [result, expResult]}, to: "double"}}}
        }
    };
    const res2 = coll.aggregate(pipeline2).toArray();
    const {difference} = res2[0];
    assert.lt(difference, 0.00000005);
    assert(coll.drop());
}

function testErrorCode(op, val, expErrorCode) {
    const nonOptimizedOp = convertToOpOnDocument(op, val);
    const pipeline = [{$project: {_id: 0, result: nonOptimizedOp}}];
    assertErrorCode(coll, pipeline, expErrorCode);
    assert(coll.drop());
}

// Simple successful int input.
testOp("$acos", NumberInt(1), 0);
testOp("$acosh", NumberInt(1), 0);
testOp("$asin", NumberInt(0), 0);
testOp("$asinh", NumberInt(0), 0);
testOp("$atan", NumberInt(0), 0);
testOp("$atan2", [NumberInt(0), NumberInt(1)], 0);
testOp("$atan2", [NumberInt(0), NumberInt(0)], 0);
testOp("$atanh", NumberInt(0), 0);
testOp("$cos", NumberInt(0), 1);
testOp("$cosh", NumberInt(0), 1);
testOp("$sin", NumberInt(0), 0);
testOp("$sinh", NumberInt(0), 0);
testOp("$tan", NumberInt(0), 0);
testOp("$tanh", NumberInt(0), 0);
testOp("$degreesToRadians", NumberInt(0), 0);
testOp("$radiansToDegrees", NumberInt(0), 0);

// Simple successful long input.
testOp("$acos", NumberLong(1), 0);
testOp("$acosh", NumberLong(1), 0);
testOp("$asin", NumberLong(0), 0);
testOp("$asinh", NumberLong(0), 0);
testOp("$atan", NumberLong(0), 0);
testOp("$atan2", [NumberLong(0), NumberLong(1)], 0);
testOp("$atan2", [NumberLong(0), NumberLong(0)], 0);
testOp("$atanh", NumberLong(0), 0);
testOp("$cos", NumberLong(0), 1);
testOp("$cosh", NumberLong(0), 1);
testOp("$sin", NumberLong(0), 0);
testOp("$sinh", NumberLong(0), 0);
testOp("$tan", NumberLong(0), 0);
testOp("$tanh", NumberLong(0), 0);
testOp("$degreesToRadians", NumberLong(0), 0);
testOp("$radiansToDegrees", NumberLong(0), 0);

// Simple successful double input.
testOp("$acos", 1, 0);
testOp("$acosh", 1, 0);
testOp("$asin", 0, 0);
testOp("$asinh", 0, 0);
testOp("$atan", 0, 0);
testOp("$atan2", [0, 1], 0);
testOp("$atan2", [0, 0], 0);
testOp("$atanh", 0, 0);
testOp("$cos", 0, 1);
testOp("$cosh", 0, 1);
testOp("$sin", 0, 0);
testOp("$sinh", 0, 0);
testOp("$tan", 0, 0);
testOp("$tanh", 0, 0);
testOp("$degreesToRadians", 0, 0);
testOp("$radiansToDegrees", 0, 0);

// Simple successful decimal input.
testOpApprox("$acos", NumberDecimal(1), NumberDecimal(0));
testOpApprox("$acosh", NumberDecimal(1), NumberDecimal(0));
testOpApprox("$asin", NumberDecimal(0), NumberDecimal(0));
testOpApprox("$asinh", NumberDecimal(0), NumberDecimal(0));
testOpApprox("$atan", NumberDecimal(0), NumberDecimal(0));
testOpApprox("$atan2", [NumberDecimal(0), 1], NumberDecimal(0));
testOpApprox("$atan2", [NumberDecimal(0), 0], NumberDecimal(0));
testOpApprox("$atanh", NumberDecimal(0), NumberDecimal(0));
testOpApprox("$cos", NumberDecimal(0), NumberDecimal(1));
testOpApprox("$cosh", NumberDecimal(0), NumberDecimal(1));
testOpApprox("$sin", NumberDecimal(0), NumberDecimal(0));
testOpApprox("$sinh", NumberDecimal(0), NumberDecimal(0));
testOpApprox("$tan", NumberDecimal(0), NumberDecimal(0));
testOpApprox("$tanh", NumberDecimal(0), NumberDecimal(0));
testOpApprox("$degreesToRadians", NumberDecimal(0), NumberDecimal(0));
testOpApprox("$radiansToDegrees", NumberDecimal(0), NumberDecimal(0));

// Infinity input produces out of bounds error.
testErrorCode("$acos", -Infinity, 50989);
testErrorCode("$acos", NumberDecimal('-Infinity'), 50989);
testErrorCode("$acos", Infinity, 50989);
testErrorCode("$acos", NumberDecimal('Infinity'), 50989);

testErrorCode("$acosh", -Infinity, 50989);
testErrorCode("$acosh", NumberDecimal('-Infinity'), 50989);

testErrorCode("$asin", -Infinity, 50989);
testErrorCode("$asin", NumberDecimal('-Infinity'), 50989);
testErrorCode("$asin", Infinity, 50989);
testErrorCode("$asin", NumberDecimal('Infinity'), 50989);

testErrorCode("$atanh", -Infinity, 50989);
testErrorCode("$atanh", NumberDecimal('-Infinity'), 50989);
testErrorCode("$atanh", Infinity, 50989);
testErrorCode("$atanh", NumberDecimal('Infinity'), 50989);

testErrorCode("$cos", -Infinity, 50989);
testErrorCode("$cos", NumberDecimal('-Infinity'), 50989);
testErrorCode("$cos", Infinity, 50989);
testErrorCode("$cos", NumberDecimal('Infinity'), 50989);

testErrorCode("$sin", -Infinity, 50989);
testErrorCode("$sin", NumberDecimal('-Infinity'), 50989);
testErrorCode("$sin", Infinity, 50989);
testErrorCode("$sin", NumberDecimal('Infinity'), 50989);

testErrorCode("$tan", -Infinity, 50989);
testErrorCode("$tan", NumberDecimal('-Infinity'), 50989);
testErrorCode("$tan", Infinity, 50989);
testErrorCode("$tan", NumberDecimal('Infinity'), 50989);

// Infinity input produces Infinity as output.
testOp("$acosh", NumberDecimal('Infinity'), NumberDecimal('Infinity'));
testOp("$acosh", Infinity, Infinity);

testOp("$asinh", NumberDecimal('Infinity'), NumberDecimal('Infinity'));
testOp("$asinh", NumberDecimal('-Infinity'), NumberDecimal('-Infinity'));
testOp("$asinh", Infinity, Infinity);
testOp("$asinh", -Infinity, -Infinity);
testOp("$cosh", NumberDecimal('Infinity'), NumberDecimal('Infinity'));
testOp("$cosh", NumberDecimal('-Infinity'), NumberDecimal('Infinity'));
testOp("$cosh", Infinity, Infinity);
testOp("$cosh", -Infinity, Infinity);
testOp("$sinh", NumberDecimal('Infinity'), NumberDecimal('Infinity'));
testOp("$sinh", NumberDecimal('-Infinity'), NumberDecimal('-Infinity'));
testOp("$sinh", Infinity, Infinity);
testOp("$sinh", -Infinity, -Infinity);

// Infinity produces finite output (due to asymptotic bounds).
testOpApprox("$atan", NumberDecimal('Infinity'), NumberDecimal(Math.PI / 2));
testOpApprox("$atan", NumberDecimal('-Infinity'), NumberDecimal(-Math.PI / 2));
testOpApprox("$atan", Infinity, Math.PI / 2);
testOpApprox("$atan", -Infinity, -Math.PI / 2);

testOpApprox("$atan2", [NumberDecimal('Infinity'), 0], NumberDecimal(Math.PI / 2));
testOpApprox("$atan2", [NumberDecimal('-Infinity'), 0], NumberDecimal(-Math.PI / 2));
testOpApprox(
    "$atan2", [NumberDecimal('-Infinity'), NumberDecimal("Infinity")], NumberDecimal(-Math.PI / 4));
testOpApprox("$atan2",
             [NumberDecimal('-Infinity'), NumberDecimal("-Infinity")],
             NumberDecimal(-3 * Math.PI / 4));
testOpApprox("$atan2", [NumberDecimal('0'), NumberDecimal("-Infinity")], NumberDecimal(Math.PI));
testOpApprox("$atan2", [NumberDecimal('0'), NumberDecimal("Infinity")], NumberDecimal(0));

testOp("$tanh", NumberDecimal('Infinity'), NumberDecimal('1'));
testOp("$tanh", NumberDecimal('-Infinity'), NumberDecimal('-1'));

// Finite input produces infinite outputs.
testOp("$atanh", NumberDecimal(1), NumberDecimal('Infinity'));
testOp("$atanh", NumberDecimal(-1), NumberDecimal('-Infinity'));
testOp("$atanh", 1, Infinity);
testOp("$atanh", -1, -Infinity);

testOp("$tanh", Infinity, 1);
testOp("$tanh", -Infinity, -1);

// Int argument out of bounds.
testErrorCode("$acos", NumberInt(-2), 50989);
testErrorCode("$acos", NumberInt(2), 50989);
testErrorCode("$asin", NumberInt(-2), 50989);
testErrorCode("$asin", NumberInt(2), 50989);
testErrorCode("$acosh", NumberInt(0), 50989);
testErrorCode("$atanh", NumberInt(2), 50989);
testErrorCode("$atanh", NumberInt(-2), 50989);

// Long argument out of bounds.
testErrorCode("$acos", NumberLong(-2), 50989);
testErrorCode("$acos", NumberLong(2), 50989);
testErrorCode("$asin", NumberLong(-2), 50989);
testErrorCode("$asin", NumberLong(2), 50989);
testErrorCode("$acosh", NumberLong(0), 50989);
testErrorCode("$atanh", NumberLong(2), 50989);
testErrorCode("$atanh", NumberLong(-2), 50989);

// Double argument out of bounds.
testErrorCode("$acos", -1.1, 50989);
testErrorCode("$acos", 1.1, 50989);
testErrorCode("$asin", -1.1, 50989);
testErrorCode("$asin", 1.1, 50989);
testErrorCode("$acosh", 0.9, 50989);
testErrorCode("$atanh", -1.00001, 50989);
testErrorCode("$atanh", 1.00001, 50989);

// Decimal argument out of bounds.
testErrorCode("$acos", NumberDecimal(-1.1), 50989);
testErrorCode("$acos", NumberDecimal(1.1), 50989);
testErrorCode("$asin", NumberDecimal(-1.1), 50989);
testErrorCode("$asin", NumberDecimal(1.1), 50989);
testErrorCode("$acosh", NumberDecimal(0.9), 50989);
testErrorCode("$atanh", NumberDecimal(-1.00001), 50989);
testErrorCode("$atanh", NumberDecimal(1.000001), 50989);

// Check NaN is preserved.
["$acos", "$asin", "$atan", "$cos", "$sin", "$tan"].forEach(op => {
    testOp([op], NaN, NaN);
    testOp([op], NumberDecimal(NaN), NumberDecimal(NaN));
    // Check the hyperbolic version of each function.
    testOp([op + 'h'], NaN, NaN);
    testOp([op + 'h'], NumberDecimal(NaN), NumberDecimal(NaN));
});

["$radiansToDegrees", "$degreesToRadians"].forEach(op => {
    testOp([op], NaN, NaN);
    testOp([op], NumberDecimal(NaN), NumberDecimal(NaN));
    testOp([op], -Infinity, -Infinity);
    testOp([op], NumberDecimal(-Infinity), NumberDecimal(-Infinity));
    testOp([op], Infinity, Infinity);
    testOp([op], NumberDecimal(Infinity), NumberDecimal(Infinity));
});

testOp("$atan2", [NumberDecimal('NaN'), NumberDecimal('NaN')], NumberDecimal('NaN'));
testOp("$atan2", [NumberDecimal('NaN'), NumberDecimal('0')], NumberDecimal('NaN'));
testOp("$atan2", [NumberDecimal('0'), NumberDecimal('NaN')], NumberDecimal('NaN'));

// atan2 additional testing with unknown constants
testOpApprox("$atan2", [NumberInt(3), NumberInt(2)], NumberDecimal(0.9827937232));
testOpApprox("$atan2", [NumberInt(621), NumberInt(84)], NumberDecimal(1.4363466632));

// Non-numeric input.
testErrorCode("$acos", "string", 28765);
testErrorCode("$acosh", "string", 28765);
testErrorCode("$asin", "string", 28765);
testErrorCode("$asinh", "string", 28765);
testErrorCode("$atan", "string", 28765);
testErrorCode("$atan2", ["string", "string"], 51044);
testErrorCode("$atan2", ["string", 0.0], 51044);
testErrorCode("$atan2", [0.0, "string"], 51045);
testErrorCode("$atanh", "string", 28765);
testErrorCode("$cos", "string", 28765);
testErrorCode("$cosh", "string", 28765);
testErrorCode("$sin", "string", 28765);
testErrorCode("$sinh", "string", 28765);
testErrorCode("$tan", "string", 28765);
testErrorCode("$tanh", "string", 28765);
testErrorCode("$degreesToRadians", "string", 28765);
testErrorCode("$radiansToDegrees", "string", 28765);
}());
