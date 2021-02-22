/**
 * Tests $dateTrunc expression.
 * @tags: [
 *   requires_fcv_49
 * ]
 */
(function() {
"use strict";
load("jstests/libs/sbe_assert_error_override.js");
load("jstests/libs/aggregation_pipeline_utils.js");  // For executeAggregationTestCase.

const testDB = db.getSiblingDB(jsTestName());
const coll = testDB.collection;

// Drop the test database.
assert.commandWorked(testDB.dropDatabase());

const someDate = new Date("2020-11-01T18:23:36Z");
const aggregationPipelineWithDateTrunc = [{
    $project: {
        _id: false,
        date_trunc:
            {$dateTrunc: {date: "$date", unit: "$unit", binSize: "$binSize", timezone: "$timeZone"}}
    }
}];
const aggregationPipelineWithDateTruncAndStartOfWeek = [{
    $project: {
        _id: false,
        date_trunc: {
            $dateTrunc: {
                date: "$date",
                unit: "$unit",
                binSize: "$binSize",
                timezone: "$timeZone",
                startOfWeek: "$startOfWeek"
            }
        }
    }
}];
const testCases = [
    {
        // Parameters are constants, timezone is not specified.
        pipeline: [{
            $project: {
                _id: true,
                date_trunc:
                    {$dateTrunc: {date: new Date("2020-11-01T18:23:36Z"), unit: "hour", binSize: 2}}
            }
        }],
        inputDocuments: [{_id: 1}],
        expectedResults: [{_id: 1, date_trunc: new Date("2020-11-01T18:00:00Z")}]
    },
    {
        // Parameters are field paths.
        pipeline: aggregationPipelineWithDateTruncAndStartOfWeek,
        inputDocuments: [{
            date: new Date("2021-12-01T18:23:36Z"),
            unit: "hour",
            binSize: 4,
            timeZone: "America/New_York",
            startOfWeek: "IGNORED"  // Ignored when unit is not week.
        }],
        expectedResults: [{date_trunc: new Date("2021-12-01T17:00:00Z")}]
    },
    // Expression parsing tests.
    {
        // Invalid $dateTrunc expression - not an object.
        pipeline: [{$project: {_id: false, date_trunc: {$dateTrunc: "A"}}}],
        inputDocuments: [],
        expectedErrorCode: 5439007,
    },
    {
        // Invalid $dateTrunc expression - invalid parameter.
        pipeline: [{$project: {_id: false, date_trunc: {$dateTrunc: {endDate: 1}}}}],
        inputDocuments: [],
        expectedErrorCode: 5439008,
    },
    {
        // Invalid $dateTrunc expression - missing 'date' parameter.
        pipeline: [{$project: {_id: false, date_trunc: {$dateTrunc: {unit: "week"}}}}],
        inputDocuments: [],
        expectedErrorCode: 5439009,
    },
    {
        // Invalid $dateTrunc expression - missing 'unit' parameter.
        pipeline: [{$project: {_id: false, date_trunc: {$dateTrunc: {date: someDate}}}}],
        inputDocuments: [],
        expectedErrorCode: 5439010,
    },
    // Evaluation tests.
    {
        // Default values are used for optional parameters (startOfWeek: "Sunday", binSize: 1,
        // timezone: "UTC").
        pipeline:
            [{$project: {_id: false, date_trunc: {$dateTrunc: {date: "$date", unit: "week"}}}}],
        inputDocuments: [{
            date: new Date("2021-02-15T18:23:36Z"),
        }],
        expectedResults: [{date_trunc: new Date("2021-02-14T00:00:00Z")}]
    },
    {
        // Optional parameters are accepted.
        pipeline: aggregationPipelineWithDateTruncAndStartOfWeek,
        inputDocuments: [{
            date: new Date("2000-01-30T18:00:00Z"),
            unit: "week",
            binSize: 2,
            timeZone: "America/New_York",
            startOfWeek: "mon"

        }],
        expectedResults: [{date_trunc: new Date("2000-01-17T05:00:00Z")}]
    },
    {
        // 'date' is object id.
        pipeline: [{
            $project:
                {_id: false, date_trunc: {$dateTrunc: {date: "$_id", unit: "minute", binSize: 10}}}
        }],
        inputDocuments:
            [{_id: new ObjectId("507c7f79bcf86cd7994f6c0e") /* timestamp 2012-10-15T21:26:17Z*/}],
        expectedResults: [{date_trunc: new Date("2012-10-15T21:20:00Z")}]
    },
    {
        // 'date' is a timestamp.
        pipeline: [{
            $project:
                {_id: false, date_trunc: {$dateTrunc: {date: "$ts", unit: "second", binSize: 10}}}
        }],
        inputDocuments: [{ts: new Timestamp(1350336377 /*2012-10-15T21:26:17Z*/, 0)}],
        expectedResults: [{date_trunc: new Date("2012-10-15T21:26:10Z")}]
    },
    {
        // Invalid 'date' type.
        pipeline: aggregationPipelineWithDateTrunc,
        inputDocuments: [{date: "string", unit: "hour", binSize: 1, timeZone: "UTC"}],
        expectedErrorCode: 5439012,
    },
    {
        // Missing 'date' value in the document, invalid other fields.
        pipeline: aggregationPipelineWithDateTrunc,
        inputDocuments: [{unit: "H", binSize: "string", timeZone: 1}],
        expectedResults: [{date_trunc: null}],
    },
    {
        // Null 'date'.
        pipeline: aggregationPipelineWithDateTrunc,
        inputDocuments: [{date: null, unit: "hour", binSize: 1, timeZone: "UTC"}],
        expectedResults: [{date_trunc: null}],
    },
    {
        // Null 'unit'.
        pipeline: aggregationPipelineWithDateTrunc,
        inputDocuments: [{date: someDate, unit: null, binSize: 1, timeZone: "UTC"}],
        expectedResults: [{date_trunc: null}],
    },
    {
        // Missing 'unit' value in the document, invalid other fields.
        pipeline: aggregationPipelineWithDateTrunc,
        inputDocuments: [{date: "Invalid", binSize: "Invalid", timeZone: "Invalid"}],
        expectedResults: [{date_trunc: null}],
    },
    {
        // Invalid 'unit' type.
        pipeline: aggregationPipelineWithDateTrunc,
        inputDocuments: [{date: someDate, unit: 1, binSize: 1, timeZone: "UTC"}],
        expectedErrorCode: 5439013,
    },
    {
        // Invalid 'unit' value.
        pipeline: aggregationPipelineWithDateTrunc,
        inputDocuments: [{date: someDate, unit: "century", binSize: 1, timeZone: "UTC"}],
        expectedErrorCode: 5439014,
    },
    {
        // Null 'binSize'.
        pipeline: aggregationPipelineWithDateTrunc,
        inputDocuments: [{date: someDate, unit: "hour", binSize: null, timeZone: "UTC"}],
        expectedResults: [{date_trunc: null}],
    },
    {
        // Missing 'binSize' value in the document, invalid other fields.
        pipeline: aggregationPipelineWithDateTrunc,
        inputDocuments: [{date: "Invalid", unit: "invalid", timeZone: "Invalid"}],
        expectedResults: [{date_trunc: null}],
    },
    {
        // Invalid 'binSize' type - string.
        pipeline: aggregationPipelineWithDateTrunc,
        inputDocuments: [{date: someDate, unit: "hour", binSize: "Number", timeZone: "UTC"}],
        expectedErrorCode: 5439017,
    },
    {
        // Invalid 'binSize' value.
        pipeline: aggregationPipelineWithDateTrunc,
        inputDocuments: [{date: someDate, unit: "hour", binSize: 0, timeZone: "UTC"}],
        expectedErrorCode: 5439018,
    },
    {
        //  Invalid 'binSize' value - a float-point number not convertable to an integer is
        //  rejected.
        pipeline: aggregationPipelineWithDateTrunc,
        inputDocuments: [{date: someDate, unit: "hour", binSize: 2.5, timeZone: "UTC"}],
        expectedErrorCode: 5439017,
    },
    {
        // 'binSize' decimal value is accepted.
        pipeline: aggregationPipelineWithDateTrunc,
        inputDocuments: [{
            date: new Date("2020-01-30T18:59:00Z"),
            unit: "minute",
            binSize: NumberDecimal("15"),
            timeZone: "UTC"
        }],
        expectedResults: [{
            date_trunc: new Date("2020-01-30T18:45:00Z"),
        }],
    },
    {
        // 'binSize' long value is accepted.
        pipeline: aggregationPipelineWithDateTrunc,
        inputDocuments: [{
            date: new Date("2020-01-30T18:59:00Z"),
            unit: "day",
            binSize: NumberLong("1"),
            timeZone: "UTC"
        }],
        expectedResults: [{
            date_trunc: new Date("2020-01-30T00:00:00Z"),
        }],
    },
    {
        // 'binSize' int value is accepted.
        pipeline: aggregationPipelineWithDateTrunc,
        inputDocuments: [{
            date: new Date("2020-01-30T18:59:00.003Z"),
            unit: "millisecond",
            binSize: NumberInt("1000"),
            timeZone: "UTC"
        }],
        expectedResults: [{
            date_trunc: new Date("2020-01-30T18:59:00.000Z"),
        }],
    },
    {
        // Null 'timezone'.
        pipeline: aggregationPipelineWithDateTrunc,
        inputDocuments: [{date: someDate, unit: "hour", binSize: 1, timeZone: null}],
        expectedResults: [{date_trunc: null}],
    },
    {
        // Missing 'timezone' value in the document, invalid other fields.
        pipeline: aggregationPipelineWithDateTrunc,
        inputDocuments: [{date: 1, unit: "century", binSize: "1"}],
        expectedResults: [{date_trunc: null}],
    },
    {
        // Invalid 'timezone' type.
        pipeline: aggregationPipelineWithDateTrunc,
        inputDocuments: [{date: someDate, unit: "hour", binSize: 1, timeZone: 1}],
        expectedErrorCode: 40517,
    },
    {
        // Invalid 'timezone' value.
        pipeline: aggregationPipelineWithDateTrunc,
        inputDocuments: [{date: someDate, unit: "hour", binSize: 1, timeZone: "America/Invalid"}],
        expectedErrorCode: 40485,
    },
    {
        // Null 'startOfWeek'.
        pipeline: aggregationPipelineWithDateTruncAndStartOfWeek,
        inputDocuments: [{date: someDate, unit: "week", startOfWeek: null}],
        expectedResults: [{date_trunc: null}],
    },
    {
        // Missing 'startOfWeek' value in the document, invalid other fields.
        pipeline: aggregationPipelineWithDateTruncAndStartOfWeek,
        inputDocuments: [{date: 1, unit: "week", binSize: "", timeZone: 1}],
        expectedResults: [{date_trunc: null}],
    },
    {
        // Invalid 'startOfWeek' type.
        pipeline: aggregationPipelineWithDateTruncAndStartOfWeek,
        inputDocuments:
            [{date: someDate, unit: "week", binSize: 1, timeZone: "GMT", startOfWeek: 1}],
        expectedErrorCode: 5439015,
    },
    {
        // Invalid 'startOfWeek' type, unit is not the week.
        pipeline: aggregationPipelineWithDateTruncAndStartOfWeek,
        inputDocuments: [{
            date: new Date("2020-01-30T18:59:00.000Z"),
            binSize: 1,
            unit: "year",
            timeZone: "GMT",
            startOfWeek: 1
        }],
        expectedResults: [{date_trunc: new Date("2020-01-01T00:00:00.000Z")}],
    },
    {
        // Invalid 'startOfWeek' value.
        pipeline: aggregationPipelineWithDateTruncAndStartOfWeek,
        inputDocuments:
            [{date: someDate, unit: "week", binSize: 1, timeZone: "GMT", startOfWeek: "FRIDIE"}],
        expectedErrorCode: 5439016,
    }
];
testCases.forEach(testCase => executeAggregationTestCase(coll, testCase));
}());