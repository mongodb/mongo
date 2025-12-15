/**
 * Test the behavior of the includeQueryStatsMetrics option for find, aggregate, getMore, distinct,
 * count, and update.
 * @tags: [requires_fcv_80]
 *
 * TODO SERVER-84678: move this test into core once mongos supports includeQueryStatsMetrics
 */
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {isLinux} from "jstests/libs/os_helpers.js";

function assertMetricEqual(metrics, name, expectedValue) {
    if (typeof expectedValue === "undefined") {
        // If there is no exact expected value given, just assert that the metric is present.
        assert(name in metrics, name + " is missing");
    } else {
        assert.eq(metrics[name], expectedValue, name + " doesn't match");
    }
}

function assertCpuNanosMetricEqual(metrics) {
    // cpuNanos will be positive on Linux systems and negative on all other systems, since
    // the metric is only collected on Linux.
    const name = "cpuNanos";
    assert(name in metrics, ` ${name} is missing. Returned metrics: ${tojson(metrics)}`);
    if (isLinux()) {
        assert.gte(metrics[name], 0, `${name} should be positive. Returned metrics: ${tojson(metrics)}`);
        return;
    }
    assert.lte(metrics[name], 0, `${name} should be negative. Returned metrics: ${tojson(metrics)}`);
}

function assertMetricsEqual(
    obj,
    {keysExamined, docsExamined, workingTimeMillis, hasSortStage, usedDisk, fromMultiPlanner, fromPlanCache} = {},
) {
    assert(obj.hasOwnProperty("metrics"), `object is missing metrics field: ${tojson(obj)}`);
    const metrics = obj.metrics;

    assertMetricEqual(metrics, "keysExamined", keysExamined);
    assertMetricEqual(metrics, "docsExamined", docsExamined);
    assertMetricEqual(metrics, "workingTimeMillis", workingTimeMillis);
    assertCpuNanosMetricEqual(metrics);
    assertMetricEqual(metrics, "hasSortStage", hasSortStage);
    assertMetricEqual(metrics, "usedDisk", usedDisk);
    assertMetricEqual(metrics, "fromMultiPlanner", fromMultiPlanner);
    assertMetricEqual(metrics, "fromPlanCache", fromPlanCache);
    assertMetricEqual(metrics, "numInterruptChecks", undefined);
    // The following metrics exist but are 0 by default.
    assertMetricEqual(metrics, "delinquentAcquisitions", 0);
    assertMetricEqual(metrics, "totalAcquisitionDelinquencyMillis", 0);
    assertMetricEqual(metrics, "maxAcquisitionDelinquencyMillis", 0);
    assertMetricEqual(metrics, "overdueInterruptApproxMaxMillis", 0);
    // The exact value of the following metrics cannot be determined beforehand, but we can assert their existence.
    assertMetricEqual(metrics, "totalTimeQueuedMicros", undefined);
    assertMetricEqual(metrics, "totalAdmissions", undefined);
    assertMetricEqual(metrics, "wasLoadShed", undefined);
    assertMetricEqual(metrics, "wasDeprioritized", undefined);
}

function assertWriteMetricsEqual(
    obj,
    {
        keysExamined,
        docsExamined,
        workingTimeMillis,
        hasSortStage,
        usedDisk,
        fromMultiPlanner,
        fromPlanCache,
        nMatched,
        nUpserted,
        nModified,
        nDeleted,
        nInserted,
    } = {},
) {
    assertMetricsEqual(obj, {
        keysExamined,
        docsExamined,
        workingTimeMillis,
        hasSortStage,
        usedDisk,
        fromMultiPlanner,
        fromPlanCache,
    });
    const metrics = obj.metrics;
    assertMetricEqual(metrics, "nMatched", nMatched);
    assertMetricEqual(metrics, "nUpserted", nUpserted);
    assertMetricEqual(metrics, "nModified", nModified);
    assertMetricEqual(metrics, "nDeleted", nDeleted);
    assertMetricEqual(metrics, "nInserted", nInserted);
}

{
    const testDB = db.getSiblingDB("test");
    let coll = testDB[jsTestName()];
    coll.drop();

    for (let b = 0; b < 5; ++b) {
        coll.insert({a: 1, b: b});
    }

    {
        // includeQueryStatsMetrics is false, no metrics should be included.
        const result = testDB.runCommand({find: coll.getName(), filter: {a: 1}, includeQueryStatsMetrics: false});
        assert.commandWorked(result);
        const cursor = result.cursor;
        assert(!cursor.hasOwnProperty("metrics"));
    }

    {
        // Basic find command with includeQueryStatsMetrics, metrics should appear.
        const result = testDB.runCommand({find: coll.getName(), filter: {a: 1}, includeQueryStatsMetrics: true});
        assert.commandWorked(result);
        const cursor = result.cursor;
        assertMetricsEqual(cursor, {
            keysExamined: 0,
            docsExamined: 5,
            hasSortStage: false,
            usedDisk: false,
            fromMultiPlanner: false,
        });
    }

    {
        // Find command against a non-existent collection, metrics should still appear.
        let nonExistentCollection = testDB[jsTestName() + "_does_not_exist"];
        nonExistentCollection.drop();
        const result = testDB.runCommand({
            find: nonExistentCollection.getName(),
            filter: {a: 1},
            includeQueryStatsMetrics: true,
        });
        assert.commandWorked(result);
        const cursor = result.cursor;
        assert(cursor.hasOwnProperty("metrics"));
        assertMetricsEqual(cursor, {
            keysExamined: 0,
            docsExamined: 0,
            hasSortStage: false,
            usedDisk: false,
            fromMultiPlanner: false,
            fromPlanCache: false,
        });
    }

    {
        // Find command against a view - internally rewritten to an aggregate.
        var viewName = jsTestName() + "_find_view";
        assert.commandWorked(testDB.createView(viewName, coll.getName(), [{$match: {a: 1}}]));
        const result = testDB.runCommand({find: viewName, includeQueryStatsMetrics: true});
        assert.commandWorked(result);
        const cursor = result.cursor;
        assertMetricsEqual(cursor, {
            keysExamined: 0,
            docsExamined: 5,
            hasSortStage: false,
            usedDisk: false,
            fromMultiPlanner: false,
        });
    }

    {
        // Aggregation without metrics requested.
        const result = testDB.runCommand({
            aggregate: coll.getName(),
            pipeline: [{$sort: {a: 1}}],
            cursor: {},
            includeQueryStatsMetrics: false,
        });
        assert.commandWorked(result);
        const cursor = result.cursor;
        assert(!cursor.hasOwnProperty("metrics"));
    }

    {
        // Aggregation with metrics requested and sort stage.
        const result = testDB.runCommand({
            aggregate: coll.getName(),
            pipeline: [{$sort: {a: 1}}],
            cursor: {},
            includeQueryStatsMetrics: true,
        });
        assert.commandWorked(result);
        const cursor = result.cursor;
        assertMetricsEqual(cursor, {
            keysExamined: 0,
            docsExamined: 5,
            hasSortStage: true,
            usedDisk: false,
            fromMultiPlanner: false,
        });
    }

    {
        // GetMore command without metrics requested.
        const cursor = coll.find({}).batchSize(1);
        const result = testDB.runCommand({getMore: cursor.getId(), collection: coll.getName(), batchSize: 100});
        assert.commandWorked(result);
        assert(!result.cursor.hasOwnProperty("metrics"));
    }

    {
        // GetMore command with metrics requested.
        const findCursor = coll.find({}).batchSize(1);
        const result = testDB.runCommand({
            getMore: findCursor.getId(),
            collection: coll.getName(),
            batchSize: 100,
            includeQueryStatsMetrics: true,
        });
        assert.commandWorked(result);
        const cursor = result.cursor;
        assertMetricsEqual(cursor, {
            keysExamined: 0,
            docsExamined: 4,
            hasSortStage: false,
            usedDisk: false,
            fromMultiPlanner: false,
        });
    }

    if (FeatureFlagUtil.isEnabled(testDB.getMongo(), "QueryStatsCountDistinct")) {
        {
            // Distinct command without metrics requested.
            const result = testDB.runCommand({distinct: coll.getName(), key: "b", includeQueryStatsMetrics: false});
            assert.commandWorked(result);
            assert(!result.hasOwnProperty("metrics"));
        }

        {
            // Distinct command with metrics requested.
            const result = testDB.runCommand({distinct: coll.getName(), key: "b", includeQueryStatsMetrics: true});
            assert.commandWorked(result);
            assertMetricsEqual(result, {
                keysExamined: 0,
                docsExamined: 5,
                hasSortStage: false,
                usedDisk: false,
                fromMultiPlanner: false,
            });
        }

        {
            // Distinct command against a view - internally rewritten to an aggregate.
            var viewName = jsTestName() + "_distinct_view";
            assert.commandWorked(testDB.createView(viewName, coll.getName(), []));
            const result = testDB.runCommand({distinct: viewName, key: "b", includeQueryStatsMetrics: true});
            assert.commandWorked(result);
            const spillParameter = testDB.adminCommand({
                getParameter: 1,
                internalQueryEnableAggressiveSpillsInGroup: 1,
            });
            const aggressiveSpillsInGroup = spillParameter["internalQueryEnableAggressiveSpillsInGroup"];
            assertMetricsEqual(result, {
                keysExamined: 0,
                docsExamined: 5,
                hasSortStage: false,
                usedDisk: aggressiveSpillsInGroup,
                fromMultiPlanner: false,
            });
        }

        {
            // includeQueryStatsMetrics is false, no metrics should be included.
            const result = testDB.runCommand({count: coll.getName(), query: {a: 1}, includeQueryStatsMetrics: false});
            assert.commandWorked(result);
            assert(!result.hasOwnProperty("metrics"));
        }

        {
            // Basic count command with includeQueryStatsMetrics, metrics should appear.
            const result = testDB.runCommand({count: coll.getName(), query: {a: 1}, includeQueryStatsMetrics: true});
            assert.commandWorked(result);
            assertMetricsEqual(result, {
                keysExamined: 0,
                docsExamined: 5,
                hasSortStage: false,
                usedDisk: false,
                fromMultiPlanner: false,
            });
        }

        {
            // Count command against a non-existent collection, metrics should still appear.
            const collName = jsTestName() + "_does_not_exist";
            const nonExistentCollection = testDB[collName];
            nonExistentCollection.drop();
            const result = testDB.runCommand({
                count: nonExistentCollection.getName(),
                query: {a: 1},
                includeQueryStatsMetrics: true,
            });
            assert.commandWorked(result);
            assertMetricsEqual(result, {
                keysExamined: 0,
                docsExamined: 0,
                hasSortStage: false,
                usedDisk: false,
                fromMultiPlanner: false,
                fromPlanCache: false,
            });
        }
    }

    // Update command tests
    if (FeatureFlagUtil.isEnabled(testDB.getMongo(), "QueryStatsUpdateCommand")) {
        // Set up a collection for update tests.
        let updateColl = testDB[jsTestName() + "_update"];
        updateColl.drop();
        for (let i = 0; i < 5; ++i) {
            updateColl.insert({_id: i, x: i});
        }

        {
            // Update command without metrics requested - no metrics should be included.
            const result = testDB.runCommand({
                update: updateColl.getName(),
                updates: [
                    {q: {_id: 0}, u: {x: 10}},
                    {q: {_id: 1}, u: {x: 11}},
                    {q: {_id: 2}, u: {x: 12}},
                ],
            });
            assert.commandWorked(result);
            assert.eq(result.n, 3, "expected 3 documents matched");
            assert.eq(result.nModified, 3, "expected 3 documents modified");
            assert(
                !result.hasOwnProperty("queryStatsMetrics"),
                "queryStatsMetrics should not be present when not requested",
            );
        }

        {
            // Issue a command with 5 update statements, request metrics for indices 1 and 3 only.
            const result = testDB.runCommand({
                update: updateColl.getName(),
                updates: [
                    {q: {_id: 0}, u: {x: 100}},
                    {q: {_id: 1}, u: {x: 101}, includeQueryStatsMetrics: true},
                    {q: {_id: 2}, u: {x: 102}},
                    {q: {_id: 3}, u: {x: 103}, includeQueryStatsMetrics: true},
                    {q: {_id: 4}, u: {x: 104}},
                ],
            });
            assert.commandWorked(result);
            assert.eq(result.n, 5, "expected 5 documents matched");
            assert.eq(result.nModified, 5, "expected 5 documents modified");

            // Verify queryStatsMetrics is present.
            assert(
                result.hasOwnProperty("queryStatsMetrics"),
                "queryStatsMetrics should be present: " + tojson(result),
            );

            const metricsArray = result.queryStatsMetrics;
            assert.eq(metricsArray.length, 2, "expected 2 metrics entries: " + tojson(metricsArray));

            // Collect the indices that have metrics.
            const metricsIndices = new Set(metricsArray.map((m) => m.index));
            assert(metricsIndices.has(1), "expected metrics for index 1");
            assert(metricsIndices.has(3), "expected metrics for index 3");
            assert.eq(metricsIndices.size, 2, "expected exactly 2 indices with metrics");

            // Verify each metrics entry has the expected structure using the helper.
            for (const entry of metricsArray) {
                assertWriteMetricsEqual(entry, {
                    keysExamined: 1,
                    docsExamined: 1,
                    hasSortStage: false,
                    usedDisk: false,
                    fromMultiPlanner: false,
                    nMatched: 1,
                    nUpserted: 0,
                    nModified: 1,
                    nDeleted: 0,
                    nInserted: 0,
                });
            }
        }
    }
}
