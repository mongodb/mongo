/**
 * Tests $group usage of block processing for time series.
 * @tags: [
 *   requires_timeseries,
 *   does_not_support_stepdowns,
 *   # During fcv upgrade/downgrade the engine might not be what we expect.
 *   cannot_run_during_upgrade_downgrade,
 *   # "Explain of a resolved view must be executed by mongos"
 *   directly_against_shardsvrs_incompatible,
 *   # Some suites use mixed-binary cluster setup where some nodes might have the flag enabled while
 *   # others -- not. For this test we need control over whether the flag is set on the node that
 *   # ends up executing the query.
 *   assumes_standalone_mongod
 * ]
 */

import "jstests/libs/sbe_assert_error_override.js";

import {assertErrorCode} from "jstests/aggregation/extras/utils.js";
import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";
import {getEngine, getQueryPlanner, getSingleNodeExplain} from "jstests/libs/analyze_plan.js";
import {blockProcessingTestCases} from "jstests/libs/block_processing_test_cases.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js"
import {checkSbeFullyEnabled} from "jstests/libs/sbe_util.js";

TimeseriesTest.run((insert) => {
    const datePrefix = 1680912440;

    let coll = db.timeseries_group;

    const timeFieldName = 'time';
    const metaFieldName = 'measurement';

    coll.drop();
    assert.commandWorked(db.createCollection(coll.getName(), {
        timeseries: {timeField: timeFieldName, metaField: metaFieldName},
    }));

    insert(coll, {
        _id: 0,
        [timeFieldName]: new Date(datePrefix + 100),
        [metaFieldName]: "foo",
        x: 123,
        y: 73,
        z: 7,
    });
    insert(coll, {
        _id: 1,
        [timeFieldName]: new Date(datePrefix + 200),
        [metaFieldName]: "foo",
        x: 123,
        y: 42,
        z: 9,
    });
    insert(coll, {
        _id: 2,
        [timeFieldName]: new Date(datePrefix + 300),
        [metaFieldName]: "foo",
        x: 456,
        y: 11,
        z: 4,
    });
    insert(coll, {
        _id: 3,
        [timeFieldName]: new Date(datePrefix + 400),
        [metaFieldName]: "foo",
        x: 456,
        y: 99,
        z: 2,
    });
    insert(coll, {
        _id: 4,
        [timeFieldName]: new Date(datePrefix + 500),
        [metaFieldName]: "foo",

        // All fields missing.
    });

    // Block-based $group requires sbe to be fully enabled and featureFlagTimeSeriesInSbe to be set.
    const sbeFullEnabled = checkSbeFullyEnabled(db) &&
        FeatureFlagUtil.isPresentAndEnabled(db.getMongo(), 'TimeSeriesInSbe');

    function runTests(allowDiskUse, forceIncreasedSpilling) {
        assert.commandWorked(db.adminCommand({
            setParameter: 1,
            internalQuerySlotBasedExecutionHashAggForceIncreasedSpilling: forceIncreasedSpilling
        }));
        const dateUpperBound = new Date(datePrefix + 500);
        const dateLowerBound = new Date(datePrefix);

        function compareResultEntries(lhs, rhs) {
            const lhsJson = tojson(lhs);
            const rhsJson = tojson(rhs);
            return lhsJson < rhsJson ? -1 : (lhsJson > rhsJson ? 1 : 0);
        }

        const options = {allowDiskUse: allowDiskUse};
        const allowDiskUseStr = allowDiskUse ? "true" : "false";

        const testcases = blockProcessingTestCases(timeFieldName,
                                                   metaFieldName,
                                                   datePrefix,
                                                   dateUpperBound,
                                                   dateLowerBound,
                                                   sbeFullEnabled);

        const expectedResults = {
            GroupByNull: [{_id: null}],
            Min_GroupByNull: [{a: 11}],
            Min_GroupByNullAllPass: [{a: 11}],
            MinWithId_GroupByNull: [{_id: null, a: 11}],
            Max_GroupByNull: [{a: 99}],
            Max_GroupByNullAllPass: [{a: 99}],
            MaxWithId_GroupByNull: [{_id: null, a: 99}],
            MaxMinusMin_GroupByNull: [{a: 88}],
            MaxMinusMin_GroupByNullAllPass: [{a: 88}],
            MaxMinusMinWithId_GroupByNull: [{_id: null, a: 88}],
            MinAndMaxWithId_GroupByNull: [{_id: null, a: 11, b: 99}],
            Min_GroupByX: [{a: 11}, {a: 42}],
            MinWithId_GroupByX: [{_id: 123, a: 42}, {_id: 456, a: 11}],
            Max_GroupByX: [{a: 73}, {a: 99}],
            MaxWithId_GroupByX: [{_id: 123, a: 73}, {_id: 456, a: 99}],
            MaxMinusMin_GroupByX: [{a: 31}, {a: 88}],
            MaxMinusMinWithId_GroupByX: [{_id: 123, a: 31}, {_id: 456, a: 88}],
            MinAndMaxWithId_GroupByX: [{_id: 123, a: 42, b: 73}, {_id: 456, a: 11, b: 99}],
            MaxMinusMinWithId_GroupByDateTrunc: [{_id: ISODate("1970-01-20T10:00:00Z"), a: 88}],
            MinWithId_GroupByDateAdd: [
                {_id: ISODate("1970-01-20T10:55:12.640Z"), a: 73},
                {_id: ISODate("1970-01-20T10:55:12.740Z"), a: 42},
                {_id: ISODate("1970-01-20T10:55:12.840Z"), a: 11},
                {_id: ISODate("1970-01-20T10:55:12.940Z"), a: 99}
            ],
            MinWithId_GroupByDateAddAndDateDiff: [
                {_id: ISODate("2024-01-01T00:00:00.100Z"), a: 73},
                {_id: ISODate("2024-01-01T00:00:00.200Z"), a: 42},
                {_id: ISODate("2024-01-01T00:00:00.300Z"), a: 11},
                {_id: ISODate("2024-01-01T00:00:00.400Z"), a: 99}
            ],
            MinWithId_GroupByDateSubtract: [
                {_id: ISODate("1970-01-20T10:55:12.440Z"), a: 73},
                {_id: ISODate("1970-01-20T10:55:12.540Z"), a: 42},
                {_id: ISODate("1970-01-20T10:55:12.640Z"), a: 11},
                {_id: ISODate("1970-01-20T10:55:12.740Z"), a: 99}
            ],
            MinWithId_GroupByDateSubtractAndDateDiff: [
                {_id: ISODate("2023-12-31T23:59:59.600Z"), a: 99},
                {_id: ISODate("2023-12-31T23:59:59.700Z"), a: 11},
                {_id: ISODate("2023-12-31T23:59:59.800Z"), a: 42},
                {_id: ISODate("2023-12-31T23:59:59.900Z"), a: 73}
            ],
            MaxAndMinOfDateDiffWithId_GroupByNull: [{a: 100, b: 500, c: -500, d: -100}],
            MaxAndMinOfDateAddDateSubtractDateTruncWithId_GroupByNull: [{
                a: ISODate("1970-01-20T10:55:12.640Z"),
                b: ISODate("1970-01-20T10:55:12.840Z"),
                "c": ISODate("1970-01-20T10:55:12Z")
            }],
            MaxMinusMinWithId_GroupByDateTruncAndDateAdd:
                [{_id: ISODate("1970-01-20T10:00:00Z"), a: 88}],
            MaxMinusMinWithId_GroupByDateTruncAndDateSubtract:
                [{_id: ISODate("1970-01-20T10:00:00Z"), a: 88}],
            MinWithId_GroupByDateDiffAndDateAdd:
                [{_id: 200, a: 73}, {_id: 300, a: 42}, {_id: 400, a: 11}, {_id: 500, a: 99}],
            MinOfDateAddWithId_GroupByNull_MissingAmount: [{a: null}],
            MaxPlusMinWithId_GroupByDateDiff: [
                {_id: 100, a: 146},
                {_id: 200, a: 84},
                {_id: 300, a: 22},
                {_id: 400, a: 198},
                {_id: 500, a: null}
            ],
            MaxPlusMinWithId_GroupByFilteredComputedDateDiff: [{_id: 300, a: 22}],
            Min_GroupByX_NoFilter: [{a: 11}, {a: 42}, {a: null}],
            MinWithId_GroupByX_NoFilter:
                [{_id: 123, a: 42}, {_id: 456, a: 11}, {_id: null, a: null}],
            Max_GroupByX_NoFilter: [{a: 73}, {a: 99}, {a: null}],
            MaxWithId_GroupByX_NoFilter:
                [{_id: 123, a: 73}, {_id: 456, a: 99}, {_id: null, a: null}],
            MaxMinusMin_GroupByX_NoFilter: [{a: 31}, {a: 88}, {a: null}],
            MaxMinusMinWithId_GroupByX_NoFilter:
                [{_id: 123, a: 31}, {_id: 456, a: 88}, {_id: null, a: null}],
            MinAndMaxWithId_GroupByX_NoFilter:
                [{_id: 123, a: 42, b: 73}, {_id: 456, a: 11, b: 99}, {_id: null, a: null, b: null}],
            MaxMinusMinWithId_GroupByDateTrunc_NoFilter:
                [{_id: ISODate("1970-01-20T10:55:00Z"), a: 88}],
            MaxMinusMinWithId_GroupByDateTruncAndDateDiff_NoFilter: [
                {_id: {date: ISODate("1970-01-20T10:55:12.400Z"), delta: 100}, a: 0},
                {_id: {date: ISODate("1970-01-20T10:55:12.600Z"), delta: 200}, a: 0},
                {_id: {date: ISODate("1970-01-20T10:55:12.600Z"), delta: 300}, a: 0},
                {_id: {date: ISODate("1970-01-20T10:55:12.800Z"), delta: 400}, a: 0},
                {_id: {date: ISODate("1970-01-20T10:55:12.800Z"), delta: 500}, a: null}
            ],
            MaxMinusMinWithId_GroupByDateTruncAndMeta_NoFilter:
                [{_id: {date: ISODate("1970-01-20T10:55:00Z"), symbol: "foo"}, a: 88}],
            MaxMinusMinWithId_GroupByMeta_NoFilter: [{_id: "foo", a: 88}],
            Avg_GroupByX: [{a: 55}, {a: 57.5}],
            Min_GroupByXAndY: [{a: 2}, {a: 4}, {a: 7}, {a: 9}],
            Min_GroupByMetaSortKey: [{a: 11}],
            MinOfMetaSortKey_GroupByX: [{a: null}, {a: null}],
            GroupWithProjectedOutFieldInAccumulator: [{_id: null, minY: 11}],
            GroupWithProjectedOutFieldInGb: [
                {_id: 11, a: 456},
                {_id: 42, a: 123},
                {_id: 73, a: 123},
                {_id: 99, a: 456},
                {_id: null, a: null}
            ],
            GroupWithMixOfProjectedOutField: []
        };

        for (let testcase of testcases) {
            const name = testcase.name + " (allowDiskUse=" + allowDiskUseStr + ")";
            const pipeline = testcase.pipeline;
            const expectedResult = expectedResults[testcase.name];
            const expectedErrorCode = testcase.expectedErrorCode;
            const usesBlockProcessing = testcase.usesBlockProcessing;

            if (expectedResult) {
                // Issue the aggregate() query and collect the results (together with their
                // JSON representations).
                const results = coll.aggregate(pipeline, options).toArray();

                // Sort the results.
                results.sort(compareResultEntries);

                const errMsgFn = () => "Test case '" + name + "':\nExpected " +
                    tojson(expectedResult) + "\n  !=\nActual " + tojson(results);

                // Check that the expected result and actual results have the same number of
                // elements.
                assert.eq(expectedResult.length, results.length, errMsgFn);

                // Check that each entry in the expected results array matches the corresponding
                // element in the actual results array.
                for (let i = 0; i < expectedResult.length; ++i) {
                    assert.docEq(expectedResult[i], results[i], errMsgFn);
                }
            } else if (expectedErrorCode) {
                assertErrorCode(coll, pipeline, expectedErrorCode);
            }

            // Check that explain indicates block processing is being used. This is a best effort
            // check.
            const explain = coll.explain().aggregate(pipeline, options);
            const engineUsed = getEngine(explain);
            const singleNodeQueryPlanner = getQueryPlanner(getSingleNodeExplain(explain));

            function testcaseAndExplainFn(description) {
                return () => description + " for test case '" + name + "' failed with explain " +
                    tojson(singleNodeQueryPlanner);
            }

            const hasSbePlan = engineUsed === "sbe";
            const sbePlan =
                hasSbePlan ? singleNodeQueryPlanner.winningPlan.slotBasedPlan.stages : null;

            if (usesBlockProcessing) {
                // Verify that we have an SBE plan, and verify that "block_group" appears in the
                // plan.
                assert.eq(engineUsed, "sbe");

                assert(sbePlan.includes("block_group"),
                       testcaseAndExplainFn("Expected explain to use block processing"));
            } else {
                if (hasSbePlan) {
                    // If 'usesBlockProcessing' is false and we have an SBE plan, verify that
                    // "block_group" does not appear anywhere in the SBE plan.
                    assert(!sbePlan.includes("block_group"),
                           testcaseAndExplainFn("Expected explain not to use block processing"));
                }
            }
        }
    }

    // Run the tests with allowDiskUse=false.
    runTests(false /* allowDiskUse */, false);

    // Run the tests with allowDiskUse=true.
    runTests(true /* allowDiskUse */, false);

    // Run the tests with allowDiskUse=true and force spilling.
    runTests(true /* allowDiskUse */, true);
});
