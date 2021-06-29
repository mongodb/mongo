/**
 * Tests $mod match expression with NaN, Infinity and large value inputs.
 */
(function() {
"use strict";
const testDB = db.getSiblingDB(jsTestName());

function executeTestCase(collection, testCase) {
    collection.drop();

    // Insert some documents into the collection.
    const documents = testCase.inputDocuments || [];
    assert.commandWorked(collection.insert(documents));

    // Issue a 'find' command with $mod and verify the result.
    const findCommand = () =>
        collection.find({attribute: {$mod: [testCase.divisor, testCase.remainder]}}, {_id: 1})
            .sort({_id: 1})
            .toArray();
    if (testCase.hasOwnProperty("expectedError")) {
        assert.throwsWithCode(findCommand, testCase.expectedError, [], testCase);
    } else {
        assert.docEq(findCommand(), testCase.expectedResults, testCase);
    }
}

const testCases = [];

// Generate a set of test cases by combining input values, input types for divisor/remainder.
const numberConverters = [NumberDecimal, Number];
for (const value of ["NaN", "+Inf", "1e19"]) {
    for (const numberConverter of numberConverters) {
        testCases.push({
            divisor: numberConverter(value),
            remainder: NumberInt("1"),
            expectedError: ErrorCodes.BadValue,
        });
        testCases.push({
            divisor: NumberInt("1"),
            remainder: numberConverter(value),
            expectedError: ErrorCodes.BadValue,
        });
    }
}

// Tests for dividend parameter.
// Double dividend value is too large.
testCases.push(
    {inputDocuments: [{attribute: -1e19}], divisor: 1, remainder: 0, expectedResults: []});

// Decimal dividend value is too large.
testCases.push({
    inputDocuments: [{attribute: NumberDecimal("1e19")}],
    divisor: 1,
    remainder: 0,
    expectedResults: []
});

// Verify that dividend value is truncated.
testCases.push({
    inputDocuments: [
        {_id: 1, attribute: 14.5},
        {_id: 2, attribute: 14.50001},
        {_id: 3, attribute: 15.01},
        {_id: 4, attribute: NumberDecimal("14.5")},
        {_id: 5, attribute: NumberDecimal("14.50001")},
        {_id: 6, attribute: NumberDecimal("15.01")},
        {_id: 7, attribute: NumberInt("24")},
        {_id: 8, attribute: NumberLong("34")},
    ],
    divisor: 10,
    remainder: 4,
    expectedResults: [{_id: 1}, {_id: 2}, {_id: 4}, {_id: 5}, {_id: 7}, {_id: 8}],
});

// Verify that NaN, Infinity decimal/double dividend values are not matched.
testCases.push({
    inputDocuments: [
        {attribute: NumberDecimal("-NaN")},
        {attribute: NumberDecimal("NaN")},
        {attribute: NumberDecimal("-Inf")},
        {attribute: NumberDecimal("Inf")},
        {attribute: +NaN},
        {attribute: -NaN},
        {attribute: Infinity},
        {attribute: -Infinity},
    ],
    divisor: 1,
    remainder: 0,
    expectedResults: [],
});

testCases.forEach((testCase, testCaseIdx) =>
                      executeTestCase(testDB.getCollection("coll" + testCaseIdx), testCase));
})();