/**
 * Tests $dateDiff expression.
 * @tags: [
 * ]
 */
import "jstests/libs/query/sbe_assert_error_override.js";

import {executeAggregationTestCase} from "jstests/libs/query/aggregation_pipeline_utils.js";

const testDB = db.getSiblingDB(jsTestName());
const coll = testDB.collection;

// Drop the test database.
assert.commandWorked(testDB.dropDatabase());

const someDate = new Date("2020-11-01T18:23:36Z");
const aggregationPipelineWithDateDiff = [
    {
        $project: {
            _id: false,
            date_diff: {
                $dateDiff: {startDate: "$startDate", endDate: "$endDate", unit: "$unit", timezone: "$timeZone"},
            },
        },
    },
];
const aggregationPipelineWithDateDiffAndStartOfWeek = [
    {
        $project: {
            _id: false,
            date_diff: {
                $dateDiff: {
                    startDate: "$startDate",
                    endDate: "$endDate",
                    unit: "$unit",
                    timezone: "$timeZone",
                    startOfWeek: "$startOfWeek",
                },
            },
        },
    },
];
const testCases = [
    {
        // Parameters are constants, timezone is not specified.
        pipeline: [
            {
                $project: {
                    _id: true,
                    date_diff: {
                        $dateDiff: {
                            startDate: new Date("2020-11-01T18:23:36Z"),
                            endDate: new Date("2020-11-02T00:00:00Z"),
                            unit: "hour",
                        },
                    },
                },
            },
        ],
        inputDocuments: [{_id: 1}],
        expectedResults: [{_id: 1, date_diff: NumberLong("6")}],
    },
    {
        // Parameters are field paths.
        pipeline: aggregationPipelineWithDateDiffAndStartOfWeek,
        inputDocuments: [
            {
                startDate: new Date("2020-11-01T18:23:36Z"),
                endDate: new Date("2020-11-02T00:00:00Z"),
                unit: "hour",
                timeZone: "America/New_York",
                startOfWeek: "IGNORED", // Ignored when unit is not week.
            },
        ],
        expectedResults: [{date_diff: NumberLong("6")}],
    },
    {
        // Parameters are field paths, 'timezone' is not specified.
        pipeline: [
            {
                $project: {
                    _id: false,
                    date_diff: {$dateDiff: {startDate: "$startDate", endDate: "$endDate", unit: "$unit"}},
                },
            },
        ],
        inputDocuments: [
            {
                startDate: new Date("2020-11-01T18:23:36Z"),
                endDate: new Date("2020-11-02T00:00:00Z"),
                unit: "hour",
            },
        ],
        expectedResults: [{date_diff: NumberLong("6")}],
    },
    {
        // 'startDate' and 'endDate' are object ids.
        pipeline: [
            {
                $project: {
                    _id: false,
                    date_diff: {
                        $dateDiff: {
                            startDate: "$_id",
                            endDate: "$_id",
                            unit: "millisecond",
                            timezone: "America/New_York",
                        },
                    },
                },
            },
        ],
        inputDocuments: [{}],
        expectedResults: [{date_diff: NumberLong("0")}],
    },
    {
        // 'startDate' and 'endDate' are timestamps.
        pipeline: [
            {
                $project: {
                    _id: false,
                    date_diff: {$dateDiff: {startDate: "$ts", endDate: "$ts", unit: "millisecond"}},
                },
            },
        ],
        inputDocuments: [{ts: new Timestamp()}],
        expectedResults: [{date_diff: NumberLong("0")}],
    },
    {
        // Invalid 'startDate' type.
        pipeline: aggregationPipelineWithDateDiff,
        inputDocuments: [{startDate: "string", endDate: someDate, unit: "hour", timeZone: "UTC"}],
        expectedErrorCode: 5166307,
    },
    {
        // Missing 'startDate' value in the document, invalid other fields.
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
        // Missing 'endDate' value in the document, invalid other fields.
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
        // Missing 'unit' value in the document, invalid other fields.
        pipeline: aggregationPipelineWithDateDiff,
        inputDocuments: [{startDate: 1, endDate: 2, timeZone: "INVALID"}],
        expectedResults: [{date_diff: null}],
    },
    {
        // Invalid 'unit' type.
        pipeline: aggregationPipelineWithDateDiff,
        inputDocuments: [{startDate: someDate, endDate: someDate, unit: 5, timeZone: "UTC"}],
        expectedErrorCode: 5439013,
    },
    {
        // Invalid 'unit' value.
        pipeline: aggregationPipelineWithDateDiff,
        inputDocuments: [{startDate: someDate, endDate: someDate, unit: "decade", timeZone: "UTC"}],
        expectedErrorCode: ErrorCodes.FailedToParse,
    },
    {
        // Null 'timezone'.
        pipeline: aggregationPipelineWithDateDiff,
        inputDocuments: [{startDate: someDate, endDate: someDate, unit: "hour", timeZone: null}],
        expectedResults: [{date_diff: null}],
    },
    {
        // Missing 'timezone' value in the document, invalid other fields. Result could be a null
        // answer or an error code depending whether pipeline is optimized.
        pipeline: aggregationPipelineWithDateDiff,
        inputDocuments: [{startDate: 1, endDate: 2, unit: "century"}],
        expectedResults: [{date_diff: null}],
        expectedErrorCode: ErrorCodes.FailedToParse,
    },
    {
        // Invalid 'timezone' type.
        pipeline: aggregationPipelineWithDateDiff,
        inputDocuments: [{startDate: someDate, endDate: someDate, unit: "hour", timeZone: 1}],
        expectedErrorCode: 40517,
    },
    {
        // Invalid 'timezone' value.
        pipeline: aggregationPipelineWithDateDiff,
        inputDocuments: [{startDate: someDate, endDate: someDate, unit: "hour", timeZone: "America/Invalid"}],
        expectedErrorCode: 40485,
    },
    {
        // Specified 'startOfWeek'.
        pipeline: aggregationPipelineWithDateDiffAndStartOfWeek,
        inputDocuments: [
            {
                startDate: new Date("2021-01-24T18:23:36Z"), // Sunday.
                endDate: new Date("2021-01-25T02:23:36Z"), // Monday.
                unit: "week",
                timeZone: "GMT",
                startOfWeek: "MONDAY",
            },
        ],
        expectedResults: [{date_diff: NumberLong("1")}],
    },
    {
        // Specified 'startOfWeek' and timezone.
        pipeline: aggregationPipelineWithDateDiffAndStartOfWeek,
        inputDocuments: [
            {
                startDate: new Date("2021-01-17T05:00:00Z"), // Sunday in New York.
                endDate: new Date("2021-01-17T04:59:00Z"), // Saturday in New York.
                unit: "week",
                timeZone: "America/New_York",
                startOfWeek: "sunday",
            },
        ],
        expectedResults: [{date_diff: NumberLong("-1")}],
    },
    {
        // Unspecified 'startOfWeek' - defaults to Sunday.
        pipeline: [
            {
                $project: {
                    _id: false,
                    date_diff: {$dateDiff: {startDate: "$startDate", endDate: "$endDate", unit: "week"}},
                },
            },
        ],
        inputDocuments: [
            {
                startDate: new Date("2021-01-24T18:23:36Z"), // Sunday.
                endDate: new Date("2021-01-25T02:23:36Z"), // Monday.
            },
        ],
        expectedResults: [{date_diff: NumberLong("0")}],
    },
    {
        // Null 'startOfWeek'.
        pipeline: aggregationPipelineWithDateDiffAndStartOfWeek,
        inputDocuments: [{startDate: someDate, endDate: someDate, unit: "week", startOfWeek: null}],
        expectedResults: [{date_diff: null}],
    },
    {
        // Missing 'startOfWeek' value in the document, invalid other fields.
        pipeline: aggregationPipelineWithDateDiffAndStartOfWeek,
        inputDocuments: [{startDate: 1, endDate: 2, unit: "week", timeZone: 1}],
        expectedResults: [{date_diff: null}],
    },
    {
        // Invalid 'startOfWeek' type.
        pipeline: aggregationPipelineWithDateDiffAndStartOfWeek,
        inputDocuments: [{startDate: someDate, endDate: someDate, unit: "week", timeZone: "GMT", startOfWeek: 1}],
        expectedErrorCode: 5439015,
    },
    {
        // Invalid 'startOfWeek' type, unit is not the week.
        pipeline: aggregationPipelineWithDateDiffAndStartOfWeek,
        inputDocuments: [{startDate: someDate, endDate: someDate, unit: "hour", timeZone: "GMT", startOfWeek: 1}],
        expectedResults: [{date_diff: NumberLong("0")}],
    },
    {
        // Invalid 'startOfWeek' value.
        pipeline: aggregationPipelineWithDateDiffAndStartOfWeek,
        inputDocuments: [
            {
                startDate: someDate,
                endDate: someDate,
                unit: "week",
                timeZone: "GMT",
                startOfWeek: "FRIDIE",
            },
        ],
        expectedErrorCode: 5439016,
    },
];
testCases.forEach((testCase) => executeAggregationTestCase(coll, testCase));
