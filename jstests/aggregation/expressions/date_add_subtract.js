/**
 * Tests for $dateAdd and $dateSubtract expressions.
 * @tags: [
 * ]
 */

(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");        // For assertErrorCode.
load("jstests/libs/sbe_assert_error_override.js");  // For mapping of error codes in SBE.

const coll = db.date_add_subtract;
coll.drop();

assert.commandWorked(coll.insert(
    [{_id: 1, date: ISODate("2020-12-31T12:10:05"), unit: "month", timezone: "Europe/Paris"}]));

function runAndAssert(dateArithmeticsSpec, expectedResult) {
    assert.eq(expectedResult,
              coll.aggregate([{$project: {_id: 0, newDate: dateArithmeticsSpec}}]).toArray());
}

function runAndAssertErrorCode(dateArithmeticsSpec, expectedErrorCode) {
    assertErrorCode(coll, [{$project: {newDate: dateArithmeticsSpec}}], expectedErrorCode);
}

(function testDateAddWithValidInputs() {
    runAndAssert({$dateAdd: {startDate: ISODate("2020-11-30T12:10:05Z"), unit: "day", amount: 1}},
                 [{newDate: ISODate("2020-12-01T12:10:05Z")}]);

    // Test that adding with a null argument (non-existing field) results in null.
    runAndAssert({$dateAdd: {startDate: "$dateSent", unit: "day", amount: 1}}, [{newDate: null}]);

    runAndAssert({$dateAdd: {startDate: "$date", unit: "$timeunit", amount: 1}}, [{newDate: null}]);

    runAndAssert({$dateAdd: {startDate: "$date", unit: "day", amount: null}}, [{newDate: null}]);

    runAndAssert({$dateAdd: {startDate: "$date", unit: "$unit", amount: 1, timezone: null}},
                 [{newDate: null}]);

    // Test combination of null and invalid arguments.
    runAndAssert({$dateAdd: {startDate: "$dateSent", unit: "workday", amount: 1}},
                 [{newDate: null}]);

    runAndAssert({$dateAdd: {startDate: "New year day", unit: "$timeunit", amount: 1}},
                 [{newDate: null}]);

    runAndAssert(
        {$dateAdd: {startDate: "$date", unit: "workday", amount: "$amount", timezone: "Unknown"}},
        [{newDate: null}]);

    runAndAssert({$dateAdd: {startDate: "$date", unit: "$unit", amount: 1.5, timezone: null}},
                 [{newDate: null}]);

    // Tests when startDate and result date cross the DST time change in a timezone.
    runAndAssert({
        $dateAdd: {
            startDate: ISODate("2020-10-24T18:10:00Z"),
            unit: "hour",
            amount: 24,
            timezone: "Europe/Paris"
        }
    },
                 [{newDate: ISODate("2020-10-25T18:10:00Z")}]);

    // When adding units of day both the startDate and the result represent 20:10:00 in
    // Europe/Paris. The two dates have different offsets from UTC due to the change in daylight
    // savings time.
    runAndAssert({
        $dateAdd: {
            startDate: ISODate("2020-10-24T18:10:00Z"),
            unit: "day",
            amount: 1,
            timezone: "Europe/Paris"
        }
    },
                 [{newDate: ISODate("2020-10-25T19:10:00Z")}]);

    // The following tests use start date from the document field and all valid values for the
    // 'unit' argument.
    runAndAssert({$dateAdd: {startDate: "$date", unit: "year", amount: -1}},
                 [{newDate: ISODate("2019-12-31T12:10:05Z")}]);

    runAndAssert({$dateAdd: {startDate: "$date", unit: "quarter", amount: 1}},
                 [{newDate: ISODate("2021-03-31T12:10:05Z")}]);

    runAndAssert({$dateAdd: {startDate: "$date", unit: "month", amount: 2}},
                 [{newDate: ISODate("2021-02-28T12:10:05Z")}]);

    runAndAssert({$dateAdd: {startDate: "$date", unit: "week", amount: 1}},
                 [{newDate: ISODate("2021-01-07T12:10:05Z")}]);

    runAndAssert({$dateAdd: {startDate: "$date", unit: "day", amount: 1}},
                 [{newDate: ISODate("2021-01-01T12:10:05Z")}]);

    runAndAssert({$dateAdd: {startDate: "$date", unit: "hour", amount: 2}},
                 [{newDate: ISODate("2020-12-31T14:10:05Z")}]);

    runAndAssert({$dateAdd: {startDate: "$date", unit: "minute", amount: -20}},
                 [{newDate: ISODate("2020-12-31T11:50:05Z")}]);

    runAndAssert({$dateAdd: {startDate: "$date", unit: "millisecond", amount: 1050}},
                 [{newDate: ISODate("2020-12-31T12:10:06.05Z")}]);

    // Tests using the document fields for unit and timezone arguments.
    runAndAssert({$dateAdd: {startDate: "$date", unit: "$unit", amount: 1}},
                 [{newDate: ISODate("2021-01-31T12:10:05Z")}]);

    runAndAssert({$dateAdd: {startDate: "$date", unit: "month", amount: 2, timezone: "$timezone"}},
                 [{newDate: ISODate("2021-02-28T12:10:05Z")}]);
})();

(function testDateSubtractWithValidInputs() {
    runAndAssert({$dateSubtract: {startDate: "$date", unit: "year", amount: 2}},
                 [{newDate: ISODate("2018-12-31T12:10:05Z")}]);

    runAndAssert({$dateSubtract: {startDate: "$date", unit: "quarter", amount: 2}},
                 [{newDate: ISODate("2020-06-30T12:10:05Z")}]);

    runAndAssert({$dateSubtract: {startDate: "$date", unit: "day", amount: 15}},
                 [{newDate: ISODate("2020-12-16T12:10:05Z")}]);

    runAndAssert({$dateSubtract: {startDate: "$date", unit: "hour", amount: 48}},
                 [{newDate: ISODate("2020-12-29T12:10:05Z")}]);

    runAndAssert({$dateSubtract: {startDate: "$date", unit: "minute", amount: 15}},
                 [{newDate: ISODate("2020-12-31T11:55:05Z")}]);

    runAndAssert({$dateSubtract: {startDate: "$date", unit: "second", amount: 125}},
                 [{newDate: ISODate("2020-12-31T12:08:00Z")}]);

    // Test last day adjustment in UTC.
    runAndAssert({$dateSubtract: {startDate: "$date", unit: "$unit", amount: 3}},
                 [{newDate: ISODate("2020-09-30T12:10:05Z")}]);

    // Test last day adjustment and crossing DST time change in a timezone.
    runAndAssert(
        {$dateSubtract: {startDate: "$date", unit: "$unit", amount: 3, timezone: "$timezone"}},
        [{newDate: ISODate("2020-09-30T11:10:05Z")}]);

    // Test last day adjustment in the New York timezone.
    runAndAssert({
        $dateSubtract: {
            startDate: ISODate("2021-01-31T03:00:00Z"),
            unit: "month",
            amount: 2,
            timezone: "America/New_York"
        }
    },
                 [{newDate: ISODate("2020-12-01T03:00:00Z")}]);
})();

// Test combinations of $dateAdd and $dateSubtract.
(function testDateArithmetics() {
    runAndAssert({
        $dateSubtract: {
            startDate: {$dateAdd: {startDate: "$date", unit: "hour", amount: 2}},
            unit: "hour",
            amount: 2
        }
    },
                 [{newDate: ISODate("2020-12-31T12:10:05Z")}]);

    assert.eq(
        coll.aggregate([{
                $project: {newDate: {$dateSubtract: {startDate: "$date", unit: "month", amount: 1}}}
            }])
            .toArray(),
        coll.aggregate([{
                $project: {
                    newDate: {
                        $dateAdd:
                            {startDate: ISODate("2020-11-30T12:10:00Z"), unit: "second", amount: 5}
                    }
                }
            }])
            .toArray());
})();

// Tests for error codes.
(function testDateArithmeticsErrorCodes() {
    // Missing required argument startDate.
    runAndAssertErrorCode({$dateAdd: {unit: "second", amount: 120}}, 5166402);

    // Missing required argument unit.
    runAndAssertErrorCode({$dateSubtract: {startDate: "$date", amount: 1}}, 5166402);

    // Unrecognized argument 'units'.
    runAndAssertErrorCode({$dateSubtract: {startDate: "$date", units: "day", amount: 1}}, 5166401);

    // Invalid string type of startDate argument.
    runAndAssertErrorCode({$dateSubtract: {startDate: "myBirthDate", unit: "year", amount: 10}},
                          5166403);

    // Invalid numeric type of unit argument.
    runAndAssertErrorCode({$dateAdd: {startDate: "$date", unit: 1, amount: 10}}, 5166404);

    // Invalid value of unit argument.
    runAndAssertErrorCode({$dateAdd: {startDate: "$date", unit: "epoch", amount: 10}},
                          ErrorCodes.FailedToParse);

    // Invalid double type of amount argument.
    runAndAssertErrorCode({$dateSubtract: {startDate: "$date", unit: "year", amount: 1.001}},
                          5166405);

    // Overflow error of dateAdd operation due to large amount.
    runAndAssertErrorCode(
        {$dateSubtract: {startDate: "$date", unit: "month", amount: 12 * 300000000}}, 5166406);

    // Invalid 'amount' parameter error of dateAdd operation due to large amount.
    runAndAssertErrorCode(
        {$dateSubtract: {startDate: "$date", unit: "month", amount: -30000000000}}, 5976500);

    // Invalid 'amount' parameter error of dateSubtract operation: long long min value cannot be
    // negated.
    runAndAssertErrorCode(
        {$dateSubtract: {startDate: "$date", unit: "day", amount: -9223372036854775808}}, 6045000);

    // Invalid value of timezone argument.
    runAndAssertErrorCode(
        {$dateAdd: {startDate: "$date", unit: "year", amount: 1, timezone: "Unknown"}}, 40485);
})();
})();
