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
import {
    blockProcessingTestCases,
    generateMetaVals
} from "jstests/libs/block_processing_test_cases.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {
    checkSbeFullFeatureFlagEnabled,
    checkSbeStatus,
    kSbeDisabled,
    kSbeFullyEnabled
} from "jstests/libs/sbe_util.js";

TimeseriesTest.run((insert) => {
    const datePrefix = 1680912440;

    let coll = db.timeseries_group_ts;
    let collNotTs = db.timeseries_group_not_ts;

    const timeFieldName = 'time';
    const metaFieldName = 'meta';

    coll.drop();
    collNotTs.drop();

    assert.commandWorked(db.createCollection(coll.getName(), {
        timeseries: {timeField: timeFieldName, metaField: metaFieldName},
    }));

    assert.commandWorked(db.createCollection(collNotTs.getName()));

    // Populate 'coll' and 'collNotTs' with the same set of documents.
    const Inf = Infinity;
    const str = "a somewhat long string";
    const metaVals = generateMetaVals();
    const xVals = [null, undefined, 42, -12.345, NaN, "789", "antidisestablishmentarianism"];
    const yVals = [0, 73.73, -Inf, "blah", str, undefined, null];

    const zSpecialVals = [undefined, 10e10, -10e10];
    const wSpecialVals = [Inf, -Inf, NaN, str, [], {}, undefined];

    let nextId = 0;
    let nextDateOffset = 0;
    let zSeed = 1234;
    let wSeed = 5767;
    let p = 0;
    let q = 0;

    for (let i = 0; i < 5; ++i) {
        const documents = [];

        for (let meta of metaVals) {
            for (let x of xVals) {
                for (let y of yVals) {
                    let id = nextId;
                    let t = new Date(datePrefix + nextDateOffset);
                    let z = zSeed;
                    let w = wSeed;

                    z = z % 2 == 0 ? z / 2 : -(z + 1) / 2;
                    w = w % 2 == 0 ? w / 2 : -(w + 1) / 2;

                    if (zSeed % 26 == 1 && zSpecialVals.length > 0) {
                        z = zSpecialVals[0];
                        zSpecialVals.shift();
                    }

                    if (wSeed % 26 == 1 && wSpecialVals.length > 0) {
                        w = wSpecialVals[0];
                        wSpecialVals.shift();
                    }

                    let doc = {_id: id, [timeFieldName]: t, [metaFieldName]: meta};

                    if (x !== undefined) {
                        doc.x = x;
                    }
                    if (y !== undefined) {
                        doc.y = y;
                    }
                    if (z !== undefined) {
                        doc.z = z;
                    }
                    if (w !== undefined) {
                        doc.w = w;
                    }
                    doc.p = p;
                    doc.q = q;

                    documents.push(doc);

                    nextId = nextId + 1;
                    nextDateOffset = (nextDateOffset + 5) % 199;
                    zSeed = (zSeed + 997) % 9967;
                    wSeed = (wSeed + 991) % 9973;
                    q = (q + 1) % 20;
                    if (q == 0) {
                        p = p + 1;
                    }
                }
            }
        }

        insert(coll, documents);
        insert(collNotTs, documents);
    }

    // Block based $group is guarded behind (SbeFull || SbeBlockHashAgg) && TimeSeriesInSbe.
    const sbeStatus = checkSbeStatus(db);

    const sbeFullyEnabled = checkSbeFullFeatureFlagEnabled(db);
    const featureFlagsAllowBlockHashAgg =
        // SBE can't be disabled altogether.
        (sbeStatus != kSbeDisabled) &&
        // We have to allow time series queries to run in SBE.
        FeatureFlagUtil.isPresentAndEnabled(db.getMongo(), 'TimeSeriesInSbe') &&
        // Either we have SBE full or the SBE BlockHashAgg flag.
        (sbeStatus == kSbeFullyEnabled ||
         FeatureFlagUtil.isPresentAndEnabled(db.getMongo(), 'SbeBlockHashAgg'));

    function runTests(allowDiskUse, forceIncreasedSpilling) {
        assert.commandWorked(db.adminCommand({
            setParameter: 1,
            internalQuerySlotBasedExecutionHashAggForceIncreasedSpilling: forceIncreasedSpilling
        }));
        const dateUpperBound = new Date(datePrefix + 200);
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
                                                   featureFlagsAllowBlockHashAgg,
                                                   sbeFullyEnabled);

        for (let testcase of testcases) {
            const name = testcase.name + " (allowDiskUse=" + allowDiskUseStr + ")";
            const pipeline = testcase.pipeline;
            const expectedErrorCode = testcase.expectedErrorCode;
            const usesBlockProcessing = testcase.usesBlockProcessing;

            if (expectedErrorCode) {
                assertErrorCode(coll, pipeline, expectedErrorCode, "Test case failed: " + name);
            } else {
                // Issue the aggregate() query and collect the results.
                const results = coll.aggregate(pipeline, options).toArray();

                // Issue the same query to collNotTs.
                const expectedResult = collNotTs.aggregate(pipeline, options).toArray();

                // Sort the results.
                results.sort(compareResultEntries);
                expectedResult.sort(compareResultEntries);

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
                assert.eq(engineUsed, "sbe", testcaseAndExplainFn("Expected to use SBE"));

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
