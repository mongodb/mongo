// Tests for $multiply aggregation expression.
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");        // For assertErrorCode().
load("jstests/libs/sbe_assert_error_override.js");  // Override error-code-checking APIs.

const coll = db.jstests_aggregation_multiply;
coll.drop();

const binaryTestCases = [
    // Test various argument types pairs.
    {document: {left: NumberInt(10), right: NumberInt(2)}, expected: NumberInt(20)},
    {document: {left: NumberInt(10), right: 2.55}, expected: 25.5},
    {document: {left: NumberInt(10), right: NumberLong("2")}, expected: NumberLong("20")},
    {
        document: {left: NumberInt(10), right: NumberDecimal("2.55")},
        expected: NumberDecimal("25.50")
    },
    {document: {left: NumberInt(10), right: null}, expected: null},

    {document: {left: 12.5, right: NumberInt(10)}, expected: 125},
    {document: {left: 12.5, right: 2.5}, expected: 31.25},
    {document: {left: 12.5, right: NumberLong("5")}, expected: 62.5},
    {
        document: {left: 12.5, right: NumberDecimal("2.5")},
        expected: NumberDecimal("31.25000000000000")
    },
    {document: {left: 12.5, right: null}, expected: null},

    {document: {left: NumberLong("10"), right: NumberInt(2)}, expected: NumberLong("20")},
    {document: {left: NumberLong("10"), right: 2.55}, expected: 25.5},
    {document: {left: NumberLong("10"), right: NumberLong("2")}, expected: NumberLong("20")},
    {
        document: {left: NumberLong("10"), right: NumberDecimal("2.55")},
        expected: NumberDecimal("25.50")
    },
    {document: {left: NumberLong("10"), right: null}, expected: null},

    {
        document: {left: NumberDecimal("12.5"), right: NumberInt(10)},
        expected: NumberDecimal("125.0")
    },
    {
        document: {left: NumberDecimal("12.5"), right: 2.5},
        expected: NumberDecimal("31.250000000000000")
    },
    {
        document: {left: NumberDecimal("12.5"), right: NumberLong("5")},
        expected: NumberDecimal("62.5")
    },
    {
        document: {left: NumberDecimal("12.5"), right: NumberDecimal("2.5")},
        expected: NumberDecimal("31.25")
    },
    {document: {left: NumberDecimal("12.5"), right: null}, expected: null},

    {document: {left: null, right: NumberInt(2)}, expected: null},
    {document: {left: null, right: 2.55}, expected: null},
    {document: {left: null, right: NumberLong("2")}, expected: null},
    {document: {left: null, right: NumberDecimal("2.55")}, expected: null},
    {document: {left: null, right: null}, expected: null}
];

binaryTestCases.forEach(function(testCase) {
    assert.commandWorked(coll.insert(testCase.document));

    const result =
        coll.aggregate({$project: {computed: {$multiply: ["$left", "$right"]}}}).toArray();

    assert.eq(result.length, 1);
    assert.eq(result[0].computed, testCase.expected);

    assert(coll.drop());
});

const nAryTestCases = [
    {
        values: [
            NumberInt(1),
            NumberInt(2),
            NumberInt(3),
            NumberInt(4),
            NumberInt(5),
            NumberInt(6),
            NumberInt(7),
            NumberInt(8),
            NumberInt(9),
            NumberInt(10)
        ],
        expected: NumberInt(3628800)
    },
    {values: [1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 7.5, 8.5, 9.5, 10.5], expected: 13427061.1083984375},
    {
        values: [
            NumberLong(1),
            NumberLong(2),
            NumberLong(3),
            NumberLong(4),
            NumberLong(5),
            NumberLong(6),
            NumberLong(7),
            NumberLong(8),
            NumberLong(9),
            NumberLong(10)
        ],
        expected: NumberLong(3628800)
    },
    {
        values: [
            NumberDecimal("1.5"),
            NumberDecimal("2.5"),
            NumberDecimal("3.5"),
            NumberDecimal("4.5"),
            NumberDecimal("5.5"),
            NumberDecimal("6.5"),
            NumberDecimal("7.5"),
            NumberDecimal("8.5"),
            NumberDecimal("9.5"),
            NumberDecimal("10.5")
        ],
        expected: NumberDecimal("13427061.1083984375")
    },
    {values: [null, 2, 3, 4, 5, 6, 7, 8, 9, 10], expected: null},
    {values: [1, 2, 3, 4, 5, null, 7, 8, 9, 10], expected: null},
    {values: [1, 2, 3, 4, 5, 6, 7, 8, 9, null], expected: null},
];

nAryTestCases.forEach(function(testCase) {
    const document = testCase.values.reduce((doc, val, idx) => {
        doc["a" + idx] = val;
        return doc;
    }, {});
    const multiplyArguments = Array.from({length: testCase.values.length}, (_, idx) => "$a" + idx);

    assert.commandWorked(coll.insert(document));

    const result = coll.aggregate({$project: {computed: {$multiply: multiplyArguments}}}).toArray();

    assert.eq(result.length, 1);
    assert.eq(result[0].computed, testCase.expected);

    assert(coll.drop());
});

// Test error codes on incorrect use of $multiply.
const errorTestCases = [
    {document: {left: 1, right: "not a number"}, errorCode: 16555},
    {document: {left: "not a number", right: 1}, errorCode: 16555},
];

errorTestCases.forEach(function(testCase) {
    assert.commandWorked(coll.insert(testCase.document));

    assertErrorCode(
        coll, {$project: {computed: {$multiply: ["$left", "$right"]}}}, testCase.errorCode);

    assert(coll.drop());
});
}());