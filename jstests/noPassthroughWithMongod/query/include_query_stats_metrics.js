/**
 * Test the behavior of the includeQueryStatsMetrics option for find, aggregate, and getMore.
 * @tags: [requires_fcv_80]
 *
 * TODO SERVER-84678: move this test into core once mongos supports includeQueryStatsMetrics
 */

function assertMetricEqual(metrics, name, expectedValue) {
    if (typeof expectedValue === 'undefined') {
        // If there is no exact expected value given, just assert that the metric is present.
        assert(name in metrics, name + " is missing");
    } else {
        assert.eq(metrics[name], expectedValue, name + " doesn't match");
    }
}

function assertMetricsEqual(cursor, {
    keysExamined,
    docsExamined,
    workingTimeMillis,
    hasSortStage,
    usedDisk,
    fromMultiPlanner,
    fromPlanCache
} = {}) {
    assert(cursor.hasOwnProperty("metrics"), "cursor is missing metrics field");
    const metrics = cursor.metrics;

    assertMetricEqual(metrics, "keysExamined", keysExamined);
    assertMetricEqual(metrics, "docsExamined", docsExamined);
    assertMetricEqual(metrics, "workingTimeMillis", workingTimeMillis);
    assertMetricEqual(metrics, "hasSortStage", hasSortStage);
    assertMetricEqual(metrics, "usedDisk", usedDisk);
    assertMetricEqual(metrics, "fromMultiPlanner", fromMultiPlanner);
    assertMetricEqual(metrics, "fromPlanCache", fromPlanCache);
}

{
    const testDB = db.getSiblingDB('test');
    var coll = testDB[jsTestName()];
    coll.drop();

    for (let b = 0; b < 5; ++b) {
        coll.insert({a: 1, b: b});
    }

    {
        // includeQueryStatsMetrics is false, no metrics should be included.
        const result = testDB.runCommand(
            {find: coll.getName(), filter: {a: 1}, includeQueryStatsMetrics: false});
        assert.commandWorked(result);
        const cursor = result.cursor;
        assert(!cursor.hasOwnProperty("metrics"));
    }

    {
        // Basic find command with includeQueryStatsMetrics, metrics should appear.
        const result = testDB.runCommand(
            {find: coll.getName(), filter: {a: 1}, includeQueryStatsMetrics: true});
        assert.commandWorked(result);
        const cursor = result.cursor;
        assertMetricsEqual(cursor, {
            keysExamined: 0,
            docsExamined: 5,
            hasSortStage: false,
            usedDisk: false,
            fromMultiPlanner: false
        });
    }

    {
        // Find command against a non-existent collection, metrics should still appear.
        var nonExistentCollection = testDB[jsTestName() + "_does_not_exist"];
        nonExistentCollection.drop();
        const result = testDB.runCommand({
            find: nonExistentCollection.getName(),
            filter: {a: 1},
            includeQueryStatsMetrics: true
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
            fromPlanCache: false
        });
    }

    {
        // Find command against a view - internally rewritten to an aggregate.
        var viewName = jsTestName() + "_view";
        assert.commandWorked(testDB.createView(viewName, coll.getName(), [{$match: {a: 1}}]));
        const result = testDB.runCommand({find: viewName, includeQueryStatsMetrics: true});
        assert.commandWorked(result);
        const cursor = result.cursor;
        assertMetricsEqual(cursor, {
            keysExamined: 0,
            docsExamined: 5,
            hasSortStage: false,
            usedDisk: false,
            fromMultiPlanner: false
        });
    }

    {
        // Aggregation without metrics requested.
        const result = testDB.runCommand({
            aggregate: coll.getName(),
            pipeline: [{$sort: {a: 1}}],
            cursor: {},
            includeQueryStatsMetrics: false
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
            includeQueryStatsMetrics: true
        });
        assert.commandWorked(result);
        const cursor = result.cursor;
        assertMetricsEqual(cursor, {
            keysExamined: 0,
            docsExamined: 5,
            hasSortStage: true,
            usedDisk: false,
            fromMultiPlanner: false
        });
    }

    {
        // GetMore command without metrics requested.
        const cursor = coll.find({}).batchSize(1);
        const result = testDB.runCommand(
            {getMore: cursor.getId(), collection: coll.getName(), batchSize: 100});
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
            includeQueryStatsMetrics: true
        });
        assert.commandWorked(result);
        const cursor = result.cursor;
        assertMetricsEqual(cursor, {
            keysExamined: 0,
            docsExamined: 4,
            hasSortStage: false,
            usedDisk: false,
            fromMultiPlanner: false
        });
    }
}
