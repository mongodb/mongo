// Tests for $divide aggregation expression.
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");        // For assertErrorCode().
load("jstests/libs/sbe_assert_error_override.js");  // Override error-code-checking APIs.

const coll = db.jstests_aggregation_divide;
coll.drop();

const testCases = [
    // Test various argument types pairs.
    {document: {left: NumberInt(10), right: NumberInt(2)}, expected: 5},
    {document: {left: NumberInt(10), right: 2.5}, expected: 4},
    {document: {left: NumberInt(10), right: NumberLong("2")}, expected: 5},
    {document: {left: NumberInt(10), right: NumberDecimal("2.5")}, expected: NumberDecimal("4")},
    {document: {left: NumberInt(10), right: null}, expected: null},

    {document: {left: 12.5, right: NumberInt(5)}, expected: 2.5},
    {document: {left: 12.5, right: 2.5}, expected: 5},
    {document: {left: 12.5, right: NumberLong("5")}, expected: 2.5},
    {
        document: {left: 12.5, right: NumberDecimal("2.5")},
        expected: NumberDecimal("5.000000000000")
    },
    {document: {left: 12.5, right: null}, expected: null},

    {document: {left: NumberLong("10"), right: NumberInt(2)}, expected: 5},
    {document: {left: NumberLong("10"), right: 2.5}, expected: 4},
    {document: {left: NumberLong("10"), right: NumberLong("2")}, expected: 5},
    {document: {left: NumberLong("10"), right: NumberDecimal("2.5")}, expected: NumberDecimal("4")},

    {document: {left: NumberDecimal("12.5"), right: NumberInt(5)}, expected: NumberDecimal("2.5")},
    {document: {left: NumberDecimal("12.5"), right: 2.5}, expected: NumberDecimal("5")},
    {
        document: {left: NumberDecimal("12.5"), right: NumberLong("5")},
        expected: NumberDecimal("2.5")
    },
    {
        document: {left: NumberDecimal("12.5"), right: NumberDecimal("2.5")},
        expected: NumberDecimal("5")
    },
    {document: {left: NumberDecimal("12.5"), right: null}, expected: null},

    // Null divided by anything (even zero) is null.
    {document: {left: null, right: NumberInt(2)}, expected: null},
    {document: {left: null, right: 2.5}, expected: null},
    {document: {left: null, right: NumberLong("2")}, expected: null},
    {document: {left: null, right: NumberDecimal("2.5")}, expected: null},

    {document: {left: null, right: NumberInt(0)}, expected: null},
    {document: {left: null, right: 0.0}, expected: null},
    {document: {left: null, right: NumberLong("0")}, expected: null},
    {document: {left: null, right: NumberDecimal("0")}, expected: null},

    // $divide arguments are converted to double before division which causes it to loose precision.
    {document: {left: NumberLong("9223372036854775807"), right: 1}, expected: 9223372036854776000},

    // Decimal values are not converted to doubles before division and represent result accurately.
    {
        document: {left: NumberDecimal("9223372036854775807"), right: 1},
        expected: NumberDecimal("9223372036854775807")
    },
];

testCases.forEach(function(testCase) {
    assert.commandWorked(coll.insert(testCase.document));

    const result = coll.aggregate({$project: {computed: {$divide: ["$left", "$right"]}}}).toArray();

    assert.eq(result.length, 1);
    assert.eq(result[0].computed, testCase.expected);

    assert(coll.drop());
});

// Test error codes on incorrect use of $divide.
const errorTestCases = [
    {document: {left: 1, right: NumberInt(0)}, errorCode: 16608},
    {document: {left: 1, right: 0.0}, errorCode: 16608},
    {document: {left: 1, right: NumberLong("0")}, errorCode: 16608},
    {document: {left: 1, right: NumberDecimal("0")}, errorCode: 16608},

    {document: {left: 1, right: "not a number"}, errorCode: 16609},
    {document: {left: "not a number", right: 1}, errorCode: 16609},
];

errorTestCases.forEach(function(testCase) {
    assert.commandWorked(coll.insert(testCase.document));

    assertErrorCode(
        coll, {$project: {computed: {$divide: ["$left", "$right"]}}}, testCase.errorCode);

    assert(coll.drop());
});
}());
