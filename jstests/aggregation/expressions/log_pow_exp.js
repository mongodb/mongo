// SERVER-18427: Add $log, $log10, $ln, $pow, and $exp aggregation expressions.

import "jstests/libs/query/sbe_assert_error_override.js";

import {assertErrorCode} from "jstests/aggregation/extras/utils.js";

var coll = db.log_exponential_expressions;
coll.drop();
assert.commandWorked(coll.insert({_id: 0, a: 8, b: 2}));

const doubleE = 2.7182818284590452;
const decimalE = NumberDecimal("2.718281828459045235360287471352662");
const decimal1overE = NumberDecimal("0.3678794411714423215955237701614609");

// Given a double, is it an integer?
function isInteger(n) {
    return !n.toString().includes('.');
}

function isNumberDecimal(n) {
    return n.toString().includes('NumberDecimal');
}

// Helper for testing that op returns expResult.
function testOp(op, expResult, failMsg) {
    const pipeline = [{$project: {_id: 0, result: op}}];
    const result = coll.aggregate(pipeline).toArray();
    assert.eq(result.length, 1);
    if (expResult === null || isNaN(expResult) || isNumberDecimal(expResult)) {
        assert.eq(result[0].result, expResult, failMsg);
    } else {
        assert.close(result[0].result, expResult, failMsg, 12 /*places*/);
    }
}

// $log, $log10, $ln.

// Valid input: numeric/null/NaN, base positive and not equal to 1, arg positive.
//  - NumberDouble
testOp({$log: [10, 10]}, 1);
testOp({$log10: [10]}, 1);
testOp({$ln: [Math.E]}, 1);

// Different double and NumberDecimal inputs, verified manually.
const logTestCases = [
    // Base 8
    {input: 1, base: 8, doubleResult: 0, decResult: NumberDecimal("0E+33")},
    {
        input: 2.5,
        base: 8,
        doubleResult: 0.4406426982957875,
        decResult: NumberDecimal("0.4406426982957874492901064764964633")
    },
    {
        input: 7,
        base: 8,
        doubleResult: 0.9357849740192015,
        decResult: NumberDecimal("0.9357849740192013691473231057439436")
    },
    {input: 8, base: 8, doubleResult: 1, decResult: NumberDecimal("1")},
    {
        input: 64,
        base: 8,
        doubleResult: 2,
        decResult: NumberDecimal("2.000000000000000000000000000000000")
    },
    {
        input: 65,
        base: 8,
        doubleResult: 2.0074559376761516,
        decResult: NumberDecimal("2.007455937676151502755710694582028")
    },

    // Base 9, a more unusual base.
    {input: 1, base: 9, doubleResult: 0, decResult: NumberDecimal("0E+33")},
    {
        input: 2.5,
        base: 9,
        doubleResult: 0.41702188357323483,
        decResult: NumberDecimal("0.4170218835732348650487566466679398")
    },
    {
        input: 4,
        base: 9,
        doubleResult: 0.6309297535714574,
        decResult: NumberDecimal("0.6309297535714574370995271143427609")
    },
    {input: 9, base: 9, doubleResult: 1, decResult: NumberDecimal("1")},
    {
        input: 10,
        base: 9,
        doubleResult: 1.0479516371446924,
        decResult: NumberDecimal("1.047951637144692302148283761010701")
    },
    {input: 81, base: 9, doubleResult: 2, decResult: NumberDecimal("2")},
    {
        input: 82,
        base: 9,
        doubleResult: 2.0055843597957064,
        decResult: NumberDecimal("2.005584359795706389324272570756155")
    },

    // Base 12.77, an even MORE unusual base.
    {input: 1, base: 12.77, doubleResult: 0, decResult: NumberDecimal("0E+33")},
    {
        input: 2.5,
        base: 12.77,
        doubleResult: 0.3597390013391846,
        decResult: NumberDecimal("0.3597390013391846393309152124110717")
    },
    {input: 12.77, base: 12.77, doubleResult: 1, decResult: NumberDecimal("1")},
    {
        input: 13,
        base: 12.77,
        doubleResult: 1.0070082433896357,
        decResult: NumberDecimal("1.007008243389635671677137269228113")
    },
    {
        input: 163.0729,
        base: 12.77,
        doubleResult: 2,
        decResult: NumberDecimal("2.000000000000000000000000000000000")
    },
    {
        input: 170,
        base: 12.77,
        doubleResult: 2.016332738676606,
        decResult: NumberDecimal("2.016332738676605709114981994718173")
    },
];
for (const test of logTestCases) {
    // If we can cast our input, base (or both) to integer types, test them as well.
    const inputs = [test.input, NumberDecimal(test.input.toString())];
    if (isInteger(test.input)) {
        inputs.push(NumberInt(test.input), NumberLong(test.input));
    }
    const bases = [test.base, NumberDecimal(test.base.toString())];
    if (isInteger(test.base)) {
        bases.push(NumberInt(test.base), NumberLong(test.base));
    }
    for (const input of inputs) {
        for (const base of bases) {
            const hasDecimalInput = isNumberDecimal(input) || isNumberDecimal(base);
            testOp(
                {$log: [input, base]}, hasDecimalInput ? test.decResult : test.doubleResult, test);
        }
    }
}

// Base 10, using $log10
const log10TestCases = [
    {input: 1, doubleResult: 0, decResult: NumberDecimal("0")},
    {
        input: 2.5,
        doubleResult: 0.3979400086720376,
        decResult: NumberDecimal("0.3979400086720376095725222105510140")
    },
    {input: 10, doubleResult: 1, decResult: NumberDecimal("1")},
    {
        input: 11,
        doubleResult: 1.041392685158225,
        decResult: NumberDecimal("1.041392685158225040750199971243024")
    },
    {input: 100, doubleResult: 2, decResult: NumberDecimal("2")},
    {
        input: 101,
        doubleResult: 2.0043213737826426,
        decResult: NumberDecimal("2.004321373782642574275188178222938")
    },
];
for (const test of log10TestCases) {
    // If the input is an integer anyway, test with our integer types as well.
    if (isInteger(test.input)) {
        testOp({$log10: NumberInt(test.input)}, test.doubleResult, test);
        testOp({$log10: NumberLong(test.input)}, test.doubleResult, test);
    }
    testOp({$log10: test.input}, test.doubleResult, test);
    testOp({$log10: NumberDecimal(test.input.toString())}, test.decResult, test);
}

// Base `e`, using $ln.
const lnTestCases = [
    {input: 1, doubleResult: 0, decResult: NumberDecimal("0")},
    // `e` is about 2.7, so this should be close to 1.
    {
        input: 2.5,
        doubleResult: 0.9162907318741551,
        decResult: NumberDecimal("0.9162907318741550651835272117680110")
    },
    {
        input: 7,
        doubleResult: 1.9459101490553132,
        decResult: NumberDecimal("1.945910149055313305105352743443180")
    },
    {
        input: 10,
        doubleResult: 2.302585092994046,
        decResult: NumberDecimal("2.302585092994045684017991454684364")
    },
];
for (const test of lnTestCases) {
    if (isInteger(test.input)) {
        testOp({$ln: NumberInt(test.input)}, test.doubleResult, test);
        testOp({$ln: NumberLong(test.input)}, test.doubleResult, test);
    }
    testOp({$ln: test.input}, test.doubleResult, test);
    testOp({$ln: NumberDecimal(test.input.toString())}, test.decResult, test);
}

// We represent `e` differently with double and NumberDecimal, so test that here.
testOp({$ln: doubleE}, 1);
testOp({$ln: 1 / doubleE}, -1);
// The below answer is actually correct: the input is an approximation of E.
testOp({$ln: decimalE}, NumberDecimal("0.9999999999999999999999999999999998"));
testOp({$ln: decimal1overE}, NumberDecimal("-0.9999999999999999999999999999999998"));

// All types converted to doubles.
testOp({$log: [NumberLong("10"), NumberLong("10")]}, 1);
testOp({$log10: [NumberLong("10")]}, 1);
testOp({$ln: [NumberLong("1")]}, 0);
// LLONG_MAX is converted to a double.
testOp({$log: [NumberLong("9223372036854775807"), 10]}, 18.964889726830812);
// Null inputs result in null.
testOp({$log: [null, 10]}, null);
testOp({$log: [10, null]}, null);
testOp({$log: [null, NumberDecimal(10)]}, null);
testOp({$log: [NumberDecimal(10), null]}, null);
testOp({$log10: [null]}, null);
testOp({$ln: [null]}, null);
// NaN inputs result in NaN.
testOp({$log: [NaN, 10]}, NaN);
testOp({$log: [10, NaN]}, NaN);
testOp({$log: [NaN, NumberDecimal(10)]}, NaN);
testOp({$log: [NumberDecimal(10), NaN]}, NaN);
testOp({$log10: [NaN]}, NaN);
testOp({$ln: [NaN]}, NaN);

// Test that $log still works when the inputs are field path expressions, meaning that the
// expression is not eligible for constant folding.
testOp({$log: ["$a", "$b"]}, 3);

// Invalid input: non-numeric/non-null, bases not positive or equal to 1, args not positive.

// Args/bases must be numeric or null.
assertErrorCode(coll, [{$project: {log: {$log: ["string", 5]}}}], 28756);
assertErrorCode(coll, [{$project: {log: {$log: [5, "string"]}}}], 28757);
assertErrorCode(coll, [{$project: {log10: {$log10: ["string"]}}}], 28765);
assertErrorCode(coll, [{$project: {ln: {$ln: ["string"]}}}], 28765);
// Args/bases cannot equal 0.
assertErrorCode(coll, [{$project: {log: {$log: [0, 5]}}}], 28758);
assertErrorCode(coll, [{$project: {log: {$log: [5, 0]}}}], 28759);
assertErrorCode(coll, [{$project: {log10: {$log10: [0]}}}], 28761);
assertErrorCode(coll, [{$project: {ln: {$ln: [0]}}}], 28766);
assertErrorCode(coll, [{$project: {log: {$log: [NumberDecimal(0), NumberDecimal(5)]}}}], 28758);
assertErrorCode(coll, [{$project: {log: {$log: [NumberDecimal(5), NumberDecimal(0)]}}}], 28759);
assertErrorCode(coll, [{$project: {log10: {$log10: [NumberDecimal(0)]}}}], 28761);
assertErrorCode(coll, [{$project: {ln: {$ln: [NumberDecimal(0)]}}}], 28766);
// Args/bases cannot be negative.
assertErrorCode(coll, [{$project: {log: {$log: [-1, 5]}}}], 28758);
assertErrorCode(coll, [{$project: {log: {$log: [5, -1]}}}], 28759);
assertErrorCode(coll, [{$project: {log10: {$log10: [-1]}}}], 28761);
assertErrorCode(coll, [{$project: {ln: {$ln: [-1]}}}], 28766);
assertErrorCode(coll, [{$project: {log: {$log: [NumberDecimal(-1), NumberDecimal(5)]}}}], 28758);
assertErrorCode(coll, [{$project: {log: {$log: [NumberDecimal(5), NumberDecimal(-1)]}}}], 28759);
assertErrorCode(coll, [{$project: {log10: {$log10: [NumberDecimal(-1)]}}}], 28761);
assertErrorCode(coll, [{$project: {ln: {$ln: [NumberDecimal(-1)]}}}], 28766);
// Base can't equal 1.
assertErrorCode(coll, [{$project: {log: {$log: [5, 1]}}}], 28759);
assertErrorCode(coll, [{$project: {log: {$log: [NumberDecimal(5), NumberDecimal(1)]}}}], 28759);

// $pow, $exp.

// Valid input - numeric/null/NaN.

// $pow -- if either input is a double return a double.
testOp({$pow: [10, 2]}, 100);
testOp({$pow: [1 / 2, -1]}, 2);
testOp({$pow: [-2, 2]}, 4);
testOp({$pow: [NumberInt("2"), 2]}, 4);
testOp({$pow: [-2, NumberInt("2")]}, 4);
// $pow -- if either input is a NumberDecimal, return a NumberDecimal
testOp({$pow: [NumberDecimal("10.0"), -2]}, NumberDecimal("0.01000000000000000000000000000000000"));
testOp({$pow: [0.5, NumberDecimal("-1")]}, NumberDecimal("2.000000000000000000000000000000000"));
testOp({$pow: [-2, NumberDecimal("2")]}, NumberDecimal("4.000000000000000000000000000000000"));
testOp({$pow: [NumberInt("2"), NumberDecimal("2")]},
       NumberDecimal("4.000000000000000000000000000000000"));
testOp({$pow: [NumberDecimal("-2.0"), NumberInt("2")]},
       NumberDecimal("4.000000000000000000000000000000000"));
testOp({$pow: [NumberDecimal("10.0"), 2]}, NumberDecimal("100.0000000000000000000000000000000"));

// If exponent is negative and base not -1, 0, or 1, return a double.
testOp({$pow: [NumberLong("2"), NumberLong("-1")]}, 1 / 2);
testOp({$pow: [NumberInt("4"), NumberInt("-1")]}, 1 / 4);
testOp({$pow: [NumberInt("4"), NumberLong("-1")]}, 1 / 4);
testOp({$pow: [NumberInt("1"), NumberLong("-2")]}, NumberLong("1"));
testOp({$pow: [NumberInt("-1"), NumberLong("-2")]}, NumberLong("1"));
testOp({$pow: [NumberLong("-1"), NumberLong("-3")]}, NumberLong("-1"));
// If result would overflow a long, return a double.
testOp({$pow: [NumberInt("2"), NumberLong("63")]}, 9223372036854776000);
// Exact decimal result
testOp({$pow: [NumberInt("5"), NumberDecimal("-112")]},
       NumberDecimal("5192296858534827628530496329220096E-112"));

// Result would be incorrect if double were returned.
testOp({$pow: [NumberInt("3"), NumberInt("35")]}, NumberLong("50031545098999707"));

// Else if either input is a long, return a long.
testOp({$pow: [NumberInt("-2"), NumberLong("63")]}, NumberLong("-9223372036854775808"));
testOp({$pow: [NumberInt("4"), NumberLong("2")]}, NumberLong("16"));
testOp({$pow: [NumberLong("4"), NumberInt("2")]}, NumberLong("16"));
testOp({$pow: [NumberLong("4"), NumberLong("2")]}, NumberLong("16"));

// Else return an int if it fits.
testOp({$pow: [NumberInt("4"), NumberInt("2")]}, 16);

// $exp always returns doubles for non-zero non-decimal inputs, since e is a double.
testOp({$exp: [NumberInt("-1")]}, 1 / Math.E);
testOp({$exp: [NumberLong("1")]}, Math.E);
// $exp returns decimal results for decimal inputs
testOp({$exp: [NumberDecimal("-1")]}, decimal1overE);
testOp({$exp: [NumberDecimal("1")]}, decimalE);
// Null input results in null.
testOp({$pow: [null, 2]}, null);
testOp({$pow: [1 / 2, null]}, null);
testOp({$pow: [null, NumberDecimal(2)]}, null);
testOp({$pow: [NumberDecimal("0.5"), null]}, null);
testOp({$exp: [null]}, null);
// NaN input results in NaN.
testOp({$pow: [NaN, 2]}, NaN);
testOp({$pow: [1 / 2, NaN]}, NaN);
testOp({$pow: [NaN, NumberDecimal(2)]}, NumberDecimal("NaN"));
testOp({$pow: [NumberDecimal("0.5"), NaN]}, NumberDecimal("NaN"));
testOp({$exp: [NaN]}, NaN);

// Invalid inputs - non-numeric/non-null types, or 0 to a negative exponent.
assertErrorCode(coll, [{$project: {pow: {$pow: [0, NumberLong("-1")]}}}], 28764);
assertErrorCode(coll, [{$project: {pow: {$pow: ["string", 5]}}}], 28762);
assertErrorCode(coll, [{$project: {pow: {$pow: [5, "string"]}}}], 28763);
assertErrorCode(coll, [{$project: {exp: {$exp: ["string"]}}}], 28765);
assertErrorCode(coll, [{$project: {pow: {$pow: [NumberDecimal(0), NumberLong("-1")]}}}], 28764);
assertErrorCode(coll, [{$project: {pow: {$pow: ["string", NumberDecimal(5)]}}}], 28762);
