/**
 * Tests $dateDiff expression.
 * @tags: [
 *   requires_fcv_49
 * ]
 */
(function() {
"use strict";
load("jstests/libs/sbe_assert_error_override.js");

const testDB = db.getSiblingDB(jsTestName());
const coll = testDB.collection;

// Drop the test database.
assert.commandWorked(testDB.dropDatabase());

// Executes a test case that inserts documents, issues an aggregate command on a collection and
// compares the results with the expected.
function executeTestCase(testCase) {
    jsTestLog(tojson(testCase));
    assert.commandWorked(coll.remove({}));

    // Insert some documents into the collection.
    assert.commandWorked(coll.insert(testCase.inputDocuments));

    // Issue an aggregate command and verify the result.
    try {
        const actualResults = coll.aggregate(testCase.pipeline).toArray();
        assert(testCase.expectedErrorCode === undefined,
               `Expected an exception with code ${testCase.expectedErrorCode}`);
        assert.docEq(actualResults, testCase.expectedResults);
    } catch (error) {
        if (testCase.expectedErrorCode === undefined) {
            throw error;
        }
        assert.commandFailedWithCode(error, testCase.expectedErrorCode);
    }
}
const someDate = new Date("2020-11-01T18:23:36Z");
const aggregationPipelineWithDateDiff = [{
    $project: {
        _id: false,
        date_diff: {
            $dateDiff:
                {startDate: "$startDate", endDate: "$endDate", unit: "$unit", timezone: "$timeZone"}
        }
    }
}];
const testCases = [
    {
        // Parameters are constants, timezone is not specified.
        pipeline: [{
            $project: {
                _id: true,
                date_diff: {
                    $dateDiff: {
                        startDate: new Date("2020-11-01T18:23:36Z"),
                        endDate: new Date("2020-11-02T00:00:00Z"),
                        unit: "hour"
                    }
                }
            }
        }],
        inputDocuments: [{_id: 1}],
        expectedResults: [{_id: 1, date_diff: NumberLong("6")}]
    },
    {
        // Parameters are field paths.
        pipeline: aggregationPipelineWithDateDiff,
        inputDocuments: [{
            startDate: new Date("2020-11-01T18:23:36Z"),
            endDate: new Date("2020-11-02T00:00:00Z"),
            unit: "hour",
            timeZone: "America/New_York"
        }],
        expectedResults: [{date_diff: NumberLong("6")}]
    },
    {
        // Parameters are field paths, 'timezone' is not specified.
        pipeline: [{
            $project: {
                _id: false,
                date_diff:
                    {$dateDiff: {startDate: "$startDate", endDate: "$endDate", unit: "$unit"}}
            }
        }],
        inputDocuments: [{
            startDate: new Date("2020-11-01T18:23:36Z"),
            endDate: new Date("2020-11-02T00:00:00Z"),
            unit: "hour"
        }],
        expectedResults: [{date_diff: NumberLong("6")}]
    },
    {
        // 'startDate' and 'endDate' are object ids.
        pipeline: [{
            $project: {
                _id: false,
                date_diff: {
                    $dateDiff: {
                        startDate: "$_id",
                        endDate: "$_id",
                        unit: "millisecond",
                        timezone: "America/New_York"
                    }
                }
            }
        }],
        inputDocuments: [{}],
        expectedResults: [{date_diff: NumberLong("0")}]
    },
    {
        // 'startDate' and 'endDate' are timestamps.
        pipeline: [{
            $project: {
                _id: false,
                date_diff: {$dateDiff: {startDate: "$ts", endDate: "$ts", unit: "millisecond"}}
            }
        }],
        inputDocuments: [{ts: new Timestamp()}],
        expectedResults: [{date_diff: NumberLong("0")}]
    },
    {
        // Invalid 'startDate' type.
        pipeline: aggregationPipelineWithDateDiff,
        inputDocuments: [{startDate: "string", endDate: someDate, unit: "hour", timeZone: "UTC"}],
        expectedErrorCode: 5166307,
    },
    {
        // Missing 'startDate', invalid other fields.
        pipeline: aggregationPipelineWithDateDiff,
        inputDocuments: [{endDate: 1, unit: "century", timeZone: "INVALID"}],
        expectedResults: [{date_diff: null}],
    },
    {
        // Null 'startDate'.
        pipeline: aggregationPipelineWithDateDiff,
        inputDocuments: [{startDate: null, endDate: someDate, unit: "hour", timeZone: "UTC"}],
        expectedResults: [{date_diff: null}],
    },
    {
        // Invalid 'endDate' type.
        pipeline: aggregationPipelineWithDateDiff,
        inputDocuments: [{startDate: someDate, endDate: 1, unit: "hour", timeZone: "UTC"}],
        expectedErrorCode: 5166307,
    },
    {
        // Missing 'endDate', invalid other fields.
        pipeline: aggregationPipelineWithDateDiff,
        inputDocuments: [{startDate: "", unit: "epoch", timeZone: "INVALID"}],
        expectedResults: [{date_diff: null}],
    },
    {
        // Null 'unit'.
        pipeline: aggregationPipelineWithDateDiff,
        inputDocuments: [{startDate: someDate, endDate: someDate, unit: null, timeZone: "UTC"}],
        expectedResults: [{date_diff: null}],
    },
    {
        // Missing 'unit', invalid other fields.
        pipeline: aggregationPipelineWithDateDiff,
        inputDocuments: [{startDate: 1, endDate: 2, timeZone: "INVALID"}],
        expectedResults: [{date_diff: null}],
    },
    {
        // Wrong 'unit' type.
        pipeline: aggregationPipelineWithDateDiff,
        inputDocuments: [{startDate: someDate, endDate: someDate, unit: 5, timeZone: "UTC"}],
        expectedErrorCode: 5166306,
    },
    {
        // Wrong 'unit' value.
        pipeline: aggregationPipelineWithDateDiff,
        inputDocuments: [{startDate: someDate, endDate: someDate, unit: "decade", timeZone: "UTC"}],
        expectedErrorCode: 9,
    },
    {
        // Null 'timezone'.
        pipeline: aggregationPipelineWithDateDiff,
        inputDocuments: [{startDate: someDate, endDate: someDate, unit: "hour", timeZone: null}],
        expectedResults: [{date_diff: null}],
    },
    {
        // Missing 'timezone', invalid other fields.
        pipeline: aggregationPipelineWithDateDiff,
        inputDocuments: [{startDate: 1, endDate: 2, unit: "century"}],
        expectedResults: [{date_diff: null}],
    },
    {
        // Wrong 'timezone' type.
        pipeline: aggregationPipelineWithDateDiff,
        inputDocuments: [{startDate: someDate, endDate: someDate, unit: "hour", timeZone: 1}],
        expectedErrorCode: 40517,
    },
    {
        // Wrong 'timezone' value.
        pipeline: aggregationPipelineWithDateDiff,
        inputDocuments:
            [{startDate: someDate, endDate: someDate, unit: "hour", timeZone: "America/Invalid"}],
        expectedErrorCode: 40485,
    }
];
testCases.forEach(executeTestCase);
}());