// SERVER-32930: Basic integration tests for trigonometric aggregation expressions.

(function() {
    "use strict";
    // For assertErrorCode.
    load("jstests/aggregation/extras/utils.js");

    const coll = db.expression_trigonometric;
    coll.drop();
    // We need at least one document in the collection in order to test expressions, add it here.
    assert.commandWorked(coll.insert({}));

    // Helper for testing that op returns expResult.
    function testOp(op, expResult) {
        const pipeline = [{$project: {_id: 0, result: op}}];
        assert.eq(coll.aggregate(pipeline).toArray(), [{result: expResult}]);
    }

    // Helper for testing that the aggregation expression 'op' returns expResult, approximately,
    // since NumberDecimal has so many representations for a given number (0 versus 0e-40 for
    // instance).
    function testOpApprox(op, expResult) {
        const pipeline = [{$project: {_id: 0, result: {$abs: {$subtract: [op, expResult]}}}}];
        assert.lt(coll.aggregate(pipeline).toArray(), [{result: NumberDecimal("0.00000005")}]);
    }

    // Simple successful int input.
    testOp({$acos: NumberInt(1)}, 0);
    testOp({$acosh: NumberInt(1)}, 0);
    testOp({$asin: NumberInt(0)}, 0);
    testOp({$asinh: NumberInt(0)}, 0);
    testOp({$atan: NumberInt(0)}, 0);
    testOp({$atan2: [NumberInt(0), NumberInt(1)]}, 0);
    testOp({$atan2: [NumberInt(0), NumberInt(0)]}, 0);
    testOp({$atanh: NumberInt(0)}, 0);
    testOp({$cos: NumberInt(0)}, 1);
    testOp({$cosh: NumberInt(0)}, 1);
    testOp({$sin: NumberInt(0)}, 0);
    testOp({$sinh: NumberInt(0)}, 0);
    testOp({$tan: NumberInt(0)}, 0);
    testOp({$tanh: NumberInt(0)}, 0);
    testOp({$degreesToRadians: NumberInt(0)}, 0);
    testOp({$radiansToDegrees: NumberInt(0)}, 0);

    // Simple successful long input.
    testOp({$acos: NumberLong(1)}, 0);
    testOp({$acosh: NumberLong(1)}, 0);
    testOp({$asin: NumberLong(0)}, 0);
    testOp({$asinh: NumberLong(0)}, 0);
    testOp({$atan: NumberLong(0)}, 0);
    testOp({$atan2: [NumberLong(0), NumberLong(1)]}, 0);
    testOp({$atan2: [NumberLong(0), NumberLong(0)]}, 0);
    testOp({$atanh: NumberLong(0)}, 0);
    testOp({$cos: NumberLong(0)}, 1);
    testOp({$cosh: NumberLong(0)}, 1);
    testOp({$sin: NumberLong(0)}, 0);
    testOp({$sinh: NumberLong(0)}, 0);
    testOp({$tan: NumberLong(0)}, 0);
    testOp({$tanh: NumberLong(0)}, 0);
    testOp({$degreesToRadians: NumberLong(0)}, 0);
    testOp({$radiansToDegrees: NumberLong(0)}, 0);

    // Simple successful double input.
    testOp({$acos: 1}, 0);
    testOp({$acosh: 1}, 0);
    testOp({$asin: 0}, 0);
    testOp({$asinh: 0}, 0);
    testOp({$atan: 0}, 0);
    testOp({$atan2: [0, 1]}, 0);
    testOp({$atan2: [0, 0]}, 0);
    testOp({$atanh: 0}, 0);
    testOp({$cos: 0}, 1);
    testOp({$cosh: 0}, 1);
    testOp({$sin: 0}, 0);
    testOp({$sinh: 0}, 0);
    testOp({$tan: 0}, 0);
    testOp({$tanh: 0}, 0);
    testOp({$degreesToRadians: 0}, 0);
    testOp({$radiansToDegrees: 0}, 0);

    // Simple successful decimal input.
    testOpApprox({$acos: NumberDecimal(1)}, NumberDecimal(0));
    testOpApprox({$acosh: NumberDecimal(1)}, NumberDecimal(0));
    testOpApprox({$asin: NumberDecimal(0)}, NumberDecimal(0));
    testOpApprox({$asinh: NumberDecimal(0)}, NumberDecimal(0));
    testOpApprox({$atan: NumberDecimal(0)}, NumberDecimal(0));
    testOpApprox({$atan2: [NumberDecimal(0), 1]}, NumberDecimal(0));
    testOpApprox({$atan2: [NumberDecimal(0), 0]}, NumberDecimal(0));
    testOpApprox({$atanh: NumberDecimal(0)}, NumberDecimal(0));
    testOpApprox({$cos: NumberDecimal(0)}, NumberDecimal(1));
    testOpApprox({$cosh: NumberDecimal(0)}, NumberDecimal(1));
    testOpApprox({$sin: NumberDecimal(0)}, NumberDecimal(0));
    testOpApprox({$sinh: NumberDecimal(0)}, NumberDecimal(0));
    testOpApprox({$tan: NumberDecimal(0)}, NumberDecimal(0));
    testOpApprox({$tanh: NumberDecimal(0)}, NumberDecimal(0));
    testOpApprox({$degreesToRadians: NumberDecimal(0)}, NumberDecimal(0));
    testOpApprox({$radiansToDegrees: NumberDecimal(0)}, NumberDecimal(0));

    // Infinity input produces out of bounds error.
    assertErrorCode(coll, [{$project: {a: {$acos: -Infinity}}}], 50989);
    assertErrorCode(coll, [{$project: {a: {$acos: NumberDecimal('-Infinity')}}}], 50989);
    assertErrorCode(coll, [{$project: {a: {$acos: Infinity}}}], 50989);
    assertErrorCode(coll, [{$project: {a: {$acos: NumberDecimal('Infinity')}}}], 50989);

    assertErrorCode(coll, [{$project: {a: {$acosh: -Infinity}}}], 50989);
    assertErrorCode(coll, [{$project: {a: {$acosh: NumberDecimal('-Infinity')}}}], 50989);

    assertErrorCode(coll, [{$project: {a: {$asin: -Infinity}}}], 50989);
    assertErrorCode(coll, [{$project: {a: {$asin: NumberDecimal('-Infinity')}}}], 50989);
    assertErrorCode(coll, [{$project: {a: {$asin: Infinity}}}], 50989);
    assertErrorCode(coll, [{$project: {a: {$asin: NumberDecimal('Infinity')}}}], 50989);

    assertErrorCode(coll, [{$project: {a: {$atanh: -Infinity}}}], 50989);
    assertErrorCode(coll, [{$project: {a: {$atanh: NumberDecimal('-Infinity')}}}], 50989);
    assertErrorCode(coll, [{$project: {a: {$atanh: Infinity}}}], 50989);
    assertErrorCode(coll, [{$project: {a: {$atanh: NumberDecimal('Infinity')}}}], 50989);

    assertErrorCode(coll, [{$project: {a: {$cos: -Infinity}}}], 50989);
    assertErrorCode(coll, [{$project: {a: {$cos: NumberDecimal('-Infinity')}}}], 50989);
    assertErrorCode(coll, [{$project: {a: {$cos: Infinity}}}], 50989);
    assertErrorCode(coll, [{$project: {a: {$cos: NumberDecimal('Infinity')}}}], 50989);

    assertErrorCode(coll, [{$project: {a: {$sin: -Infinity}}}], 50989);
    assertErrorCode(coll, [{$project: {a: {$sin: NumberDecimal('-Infinity')}}}], 50989);
    assertErrorCode(coll, [{$project: {a: {$sin: Infinity}}}], 50989);
    assertErrorCode(coll, [{$project: {a: {$sin: NumberDecimal('Infinity')}}}], 50989);

    assertErrorCode(coll, [{$project: {a: {$tan: -Infinity}}}], 50989);
    assertErrorCode(coll, [{$project: {a: {$tan: NumberDecimal('-Infinity')}}}], 50989);
    assertErrorCode(coll, [{$project: {a: {$tan: Infinity}}}], 50989);
    assertErrorCode(coll, [{$project: {a: {$tan: NumberDecimal('Infinity')}}}], 50989);

    // Infinity input produces Infinity as output.
    testOp({$acosh: NumberDecimal('Infinity')}, NumberDecimal('Infinity'));
    testOp({$acosh: Infinity}, Infinity);

    testOp({$asinh: NumberDecimal('Infinity')}, NumberDecimal('Infinity'));
    testOp({$asinh: NumberDecimal('-Infinity')}, NumberDecimal('-Infinity'));
    testOp({$asinh: Infinity}, Infinity);
    testOp({$asinh: -Infinity}, -Infinity);
    testOp({$cosh: NumberDecimal('Infinity')}, NumberDecimal('Infinity'));
    testOp({$cosh: NumberDecimal('-Infinity')}, NumberDecimal('Infinity'));
    testOp({$cosh: Infinity}, Infinity);
    testOp({$cosh: -Infinity}, Infinity);
    testOp({$sinh: NumberDecimal('Infinity')}, NumberDecimal('Infinity'));
    testOp({$sinh: NumberDecimal('-Infinity')}, NumberDecimal('-Infinity'));
    testOp({$sinh: Infinity}, Infinity);
    testOp({$sinh: -Infinity}, -Infinity);

    // Infinity produces finite output (due to asymptotic bounds).
    testOpApprox({$atan: NumberDecimal('Infinity')}, NumberDecimal(Math.PI / 2));
    testOpApprox({$atan: NumberDecimal('-Infinity')}, NumberDecimal(Math.Pi / 2));
    testOpApprox({$atan: Infinity}, Math.PI / 2);
    testOpApprox({$atan: -Infinity}, -Math.PI / 2);

    testOpApprox({$atan2: [NumberDecimal('Infinity'), 0]}, NumberDecimal(Math.PI / 2));
    testOpApprox({$atan2: [NumberDecimal('-Infinity'), 0]}, NumberDecimal(-Math.PI / 2));
    testOpApprox({$atan2: [NumberDecimal('-Infinity'), NumberDecimal("Infinity")]},
                 NumberDecimal(-Math.PI / 4));
    testOpApprox({$atan2: [NumberDecimal('-Infinity'), NumberDecimal("-Infinity")]},
                 NumberDecimal(-3 * Math.PI / 4));
    testOpApprox({$atan2: [NumberDecimal('0'), NumberDecimal("-Infinity")]},
                 NumberDecimal(Math.PI));
    testOpApprox({$atan2: [NumberDecimal('0'), NumberDecimal("Infinity")]}, NumberDecimal(0));

    testOp({$tanh: NumberDecimal('Infinity')}, NumberDecimal('1'));
    testOp({$tanh: NumberDecimal('-Infinity')}, NumberDecimal('-1'));

    // Finite input produces infinite outputs.
    testOp({$atanh: NumberDecimal(1)}, NumberDecimal('Infinity'));
    testOp({$atanh: NumberDecimal(-1)}, NumberDecimal('-Infinity'));
    testOp({$atanh: 1}, Infinity);
    testOp({$atanh: -1}, -Infinity);

    testOp({$tanh: Infinity}, 1);
    testOp({$tanh: -Infinity}, -1);

    // Int argument out of bounds.
    assertErrorCode(coll, [{$project: {a: {$acos: NumberInt(-2)}}}], 50989);
    assertErrorCode(coll, [{$project: {a: {$acos: NumberInt(2)}}}], 50989);
    assertErrorCode(coll, [{$project: {a: {$asin: NumberInt(-2)}}}], 50989);
    assertErrorCode(coll, [{$project: {a: {$asin: NumberInt(2)}}}], 50989);
    assertErrorCode(coll, [{$project: {a: {$acosh: NumberInt(0)}}}], 50989);
    assertErrorCode(coll, [{$project: {a: {$atanh: NumberInt(2)}}}], 50989);
    assertErrorCode(coll, [{$project: {a: {$atanh: NumberInt(-2)}}}], 50989);

    // Long argument out of bounds.
    assertErrorCode(coll, [{$project: {a: {$acos: NumberLong(-2)}}}], 50989);
    assertErrorCode(coll, [{$project: {a: {$acos: NumberLong(2)}}}], 50989);
    assertErrorCode(coll, [{$project: {a: {$asin: NumberLong(-2)}}}], 50989);
    assertErrorCode(coll, [{$project: {a: {$asin: NumberLong(2)}}}], 50989);
    assertErrorCode(coll, [{$project: {a: {$acosh: NumberLong(0)}}}], 50989);
    assertErrorCode(coll, [{$project: {a: {$atanh: NumberLong(2)}}}], 50989);
    assertErrorCode(coll, [{$project: {a: {$atanh: NumberLong(-2)}}}], 50989);

    // Double argument out of bounds.
    assertErrorCode(coll, [{$project: {a: {$acos: -1.1}}}], 50989);
    assertErrorCode(coll, [{$project: {a: {$acos: 1.1}}}], 50989);
    assertErrorCode(coll, [{$project: {a: {$asin: -1.1}}}], 50989);
    assertErrorCode(coll, [{$project: {a: {$asin: 1.1}}}], 50989);
    assertErrorCode(coll, [{$project: {a: {$acosh: 0.9}}}], 50989);
    assertErrorCode(coll, [{$project: {a: {$atanh: -1.00001}}}], 50989);
    assertErrorCode(coll, [{$project: {a: {$atanh: 1.00001}}}], 50989);

    // Decimal argument out of bounds.
    assertErrorCode(coll, [{$project: {a: {$acos: NumberDecimal(-1.1)}}}], 50989);
    assertErrorCode(coll, [{$project: {a: {$acos: NumberDecimal(1.1)}}}], 50989);
    assertErrorCode(coll, [{$project: {a: {$asin: NumberDecimal(-1.1)}}}], 50989);
    assertErrorCode(coll, [{$project: {a: {$asin: NumberDecimal(1.1)}}}], 50989);
    assertErrorCode(coll, [{$project: {a: {$acosh: NumberDecimal(0.9)}}}], 50989);
    assertErrorCode(coll, [{$project: {a: {$atanh: NumberDecimal(-1.00001)}}}], 50989);
    assertErrorCode(coll, [{$project: {a: {$atanh: NumberDecimal(1.000001)}}}], 50989);

    // Check NaN is preserved.
    ["$acos", "$asin", "$atan", "$cos", "$sin", "$tan"].forEach(op => {
        testOp({[op]: NaN}, NaN);
        testOp({[op]: NumberDecimal(NaN)}, NumberDecimal(NaN));
        // Check the hyperbolic version of each function.
        testOp({[op + 'h']: NaN}, NaN);
        testOp({[op + 'h']: NumberDecimal(NaN)}, NumberDecimal(NaN));
    });

    ["$radiansToDegrees", "$degreesToRadians"].forEach(op => {
        testOp({[op]: NaN}, NaN);
        testOp({[op]: NumberDecimal(NaN)}, NumberDecimal(NaN));
        testOp({[op]: -Infinity}, -Infinity);
        testOp({[op]: NumberDecimal(-Infinity)}, NumberDecimal(-Infinity));
        testOp({[op]: Infinity}, Infinity);
        testOp({[op]: NumberDecimal(Infinity)}, NumberDecimal(Infinity));
    });

    testOp({$atan2: [NumberDecimal('NaN'), NumberDecimal('NaN')]}, NumberDecimal('NaN'));
    testOp({$atan2: [NumberDecimal('NaN'), NumberDecimal('0')]}, NumberDecimal('NaN'));
    testOp({$atan2: [NumberDecimal('0'), NumberDecimal('NaN')]}, NumberDecimal('NaN'));

    // Non-numeric input.
    assertErrorCode(coll, [{$project: {a: {$acos: "string"}}}], 28765);
    assertErrorCode(coll, [{$project: {a: {$acosh: "string"}}}], 28765);
    assertErrorCode(coll, [{$project: {a: {$asin: "string"}}}], 28765);
    assertErrorCode(coll, [{$project: {a: {$asinh: "string"}}}], 28765);
    assertErrorCode(coll, [{$project: {a: {$atan: "string"}}}], 28765);
    assertErrorCode(coll, [{$project: {a: {$atan2: ["string", "string"]}}}], 51044);
    assertErrorCode(coll, [{$project: {a: {$atan2: ["string", 0.0]}}}], 51044);
    assertErrorCode(coll, [{$project: {a: {$atan2: [0.0, "string"]}}}], 51045);
    assertErrorCode(coll, [{$project: {a: {$atanh: "string"}}}], 28765);
    assertErrorCode(coll, [{$project: {a: {$cos: "string"}}}], 28765);
    assertErrorCode(coll, [{$project: {a: {$cosh: "string"}}}], 28765);
    assertErrorCode(coll, [{$project: {a: {$sin: "string"}}}], 28765);
    assertErrorCode(coll, [{$project: {a: {$sinh: "string"}}}], 28765);
    assertErrorCode(coll, [{$project: {a: {$tan: "string"}}}], 28765);
    assertErrorCode(coll, [{$project: {a: {$tanh: "string"}}}], 28765);
    assertErrorCode(coll, [{$project: {a: {$degreesToRadians: "string"}}}], 28765);
    assertErrorCode(coll, [{$project: {a: {$radiansToDegrees: "string"}}}], 28765);
}());
