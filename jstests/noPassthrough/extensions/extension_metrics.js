/**
 * Tests that the $metrics aggregation stage collects metrics and includes them in slow query logs and the profiler.
 * This is a noPassthrough version that runs against both standalone mongod and mongos with extensions loaded.
 *
 * @tags: [featureFlagExtensionsAPI]
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {
    generateExtensionConfigs,
    deleteExtensionConfigs,
    checkPlatformCompatibleWithExtensions,
} from "jstests/noPassthrough/libs/extension_helpers.js";

checkPlatformCompatibleWithExtensions();

// Generate extension configs for both metrics extensions.
const extensionNames = generateExtensionConfigs([
    "libmetrics_mongo_extension.so",
    "libother_metrics_mongo_extension.so",
]);

const options = {
    loadExtensions: extensionNames,
};

function retrieveExtensionMetricLogsFromDb(databaseConn, comment) {
    const slowQueryLogId = 51803; // ID for 'Slow query' log messages.

    return checkLog
        .getFilteredLogMessages(databaseConn, slowQueryLogId, {command: {comment: comment}}, null, true)
        .filter((log) => {
            return log.attr && log.attr.extensionMetrics !== undefined;
        });
}

// Helper function to retrieve slow query logs for a command, given a comment.
function getSlowQueryLogsByComment(db, comment, shardingTest = null, metricsOnMongoS = false) {
    if (!shardingTest || metricsOnMongoS) {
        // Logs are on the standalone mongod if the topology is unsharded.
        // Logs are on mongos if the collection is sharded AND the query ends up running the stage on mongos,
        // for example if there is a $sort preceding the extension stage.
        return retrieveExtensionMetricLogsFromDb(db, comment);
    }

    // If sharded, get logs from the shards.
    // Iterate through all shards and collect logs from their primaries.
    let logMessages = [];
    let i = 0;
    while (shardingTest["rs" + i]) {
        const shardRs = shardingTest["rs" + i];
        const shardDb = shardRs.getPrimary().getDB(db.getName());
        logMessages = logMessages.concat(retrieveExtensionMetricLogsFromDb(shardDb, comment));
        i++;
    }
    return logMessages;
}

// Helper function to extract extension metrics from a slow query log.
function sumExtensionMetricsFromSlowQueryLog(logs, extensionName, metricsName) {
    return logs.reduce((accum, log) => {
        assert(log.attr !== undefined, `Log entry was missing 'attr' field: ${tojson(log)}`);
        assert(
            log.attr.extensionMetrics !== undefined,
            `Log entry 'attr' was missing 'extensionMetrics' field: ${tojson(log.attr)}`,
        );
        assert(
            log.attr.extensionMetrics[extensionName] !== undefined,
            `Log entry 'attr.extensionMetrics' was missing '${extensionName}' field: ${tojson(log.attr.extensionMetrics)}`,
        );
        assert(
            log.attr.extensionMetrics[extensionName][metricsName] !== undefined,
            `Log entry 'attr.extensionMetrics[${extensionName}]' was missing '${metricsName}' field: ${tojson(log.attr.extensionMetrics[extensionName])}`,
        );
        return accum + log.attr.extensionMetrics[extensionName][metricsName];
    }, 0);
}

// Helper function to get profiler entries, given a comment. This function assumes that there will be only
// one aggregate command.
function getAggregateProfilerEntry(db, collName, comment) {
    const profileEntries = db.system.profile
        .aggregate([
            {
                $match: {
                    op: "command",
                    "command.aggregate": collName,
                    "command.comment": comment,
                },
            },
        ])
        .toArray();
    assert.eq(profileEntries.length, 1, `Expected exactly 1 profiler entry: ${tojson(profileEntries)}`);
    return profileEntries[0];
}

// Helper function to get profiler entries, given a comment and a cursorId. This function is used for getMore commands.
function getGetMoreProfilerEntry(db, cursorId, comment) {
    const profileEntries = db.system.profile
        .aggregate([
            {
                $match: {
                    op: "getmore",
                    "command.getMore": cursorId,
                    "command.comment": comment,
                },
            },
        ])
        .toArray();
    assert.eq(profileEntries.length, 1);
    return profileEntries[0];
}

// Helper function to extract extension metrics from a profiler entry.
function getExtensionMetricsFromProfilerEntry(entry) {
    assert(entry.extensionMetrics !== undefined, `Entry was missing 'extensionMetrics': ${tojson(entry)}`);
    return entry.extensionMetrics;
}

// Helper function that validates extension metrics in slow query logs and the profiler.
function validateExtensionMetrics(
    db,
    collName,
    comment,
    expectedValue,
    extensionName,
    metricsName,
    shardingTest = null,
    metricsOnMongoS = false,
) {
    // Verify the slow query log contains the correct metrics.
    const slowQueryLogs = getSlowQueryLogsByComment(db, comment, shardingTest, metricsOnMongoS);
    const summedSlowQueryMetrics = sumExtensionMetricsFromSlowQueryLog(slowQueryLogs, extensionName, metricsName);
    assert.eq(summedSlowQueryMetrics, expectedValue);

    if (shardingTest) {
        return;
    }

    // Verify that the profiler contains the correct metrics.
    const profilerEntry = getAggregateProfilerEntry(db, collName, comment);
    const profilerMetrics = getExtensionMetricsFromProfilerEntry(profilerEntry);
    assert(
        profilerMetrics[extensionName] !== undefined,
        `Profiler entry was missing '${extensionName}' field: ${tojson(profilerMetrics)}`,
    );
    assert(
        profilerMetrics[extensionName][metricsName] !== undefined,
        `Profiler entry '${extensionName}' was missing '${metricsName}' field: ${tojson(profilerMetrics[extensionName])}`,
    );
    assert.eq(profilerMetrics[extensionName][metricsName], expectedValue);
}

// Main test function that runs all the extension metrics tests.
function runExtensionMetricsTests(conn, shardingTest = null, shouldShardCollection = false) {
    const db = conn.getDB("test");
    const collName = jsTestName();
    const coll = db[collName];
    coll.drop();

    if (shardingTest) {
        // Note that mongos does not support the profiler so we only change the profiler level if we are running against mongod.
        // Also, set slowms on each of the shards.
        assert.commandWorked(db.runCommand({profile: 0, slowms: -1}));
        let i = 0;
        while (shardingTest["rs" + i]) {
            const shardRs = shardingTest["rs" + i];
            const shardDb = shardRs.getPrimary().getDB(db.getName());
            assert.commandWorked(shardDb.runCommand({profile: 0, slowms: -1}));
            i++;
        }
    } else {
        // Use profiling level 2 to guarantee all operations (including getMore) are logged.
        // Profiling level 2 logs ALL operations to db.system.profile, regardless of execution time.
        // Set the slow query threshold to -1 so all queries get logged.
        assert.commandWorked(db.runCommand({profile: 2, slowms: -1}));
    }

    // Insert test data.
    assert.commandWorked(coll.insert([{counter: 1}, {counter: 2}, {counter: 3}]));
    const kCounterFieldSum = 6;

    // Shard the collection if requested (for multi-shard cluster tests).
    if (shouldShardCollection && shardingTest) {
        // Create an index on the shard key before sharding.
        assert.commandWorked(coll.createIndex({counter: 1}));
        shardingTest.shardColl(coll, {counter: 1});
    }

    // Tests that a single batch aggregation query collects metrics from extensions and reports
    // them in the profiler and slow query logs.
    (function singleBatchAggregationCanCollectMetrics() {
        const comment = "single_batch_aggregation_test";
        coll.aggregate([{$metrics: {}}], {comment});

        // Metrics will be on mongod because there is no $sort in this pipeline.
        validateExtensionMetrics(db, collName, comment, kCounterFieldSum, "$metrics", "counter", shardingTest, false);
    })();

    // Tests that a query that issues multiple getMore commands can report metrics in the profiler/slow query logs.
    (function multipleGetMoresCanCollectMetrics() {
        const comment = "multiple_get_mores_test";
        const initialResult = db.runCommand({
            aggregate: collName,
            pipeline: [{$sort: {counter: 1}}, {$metrics: {}}],
            cursor: {batchSize: 1},
            comment: comment,
        });
        // Metrics should be on mongos because there is a $sort in this pipeline.
        validateExtensionMetrics(db, collName, comment, 1, "$metrics", "counter", shardingTest, shouldShardCollection);

        let cursorId = initialResult.cursor.id;
        let getMoreCounter = 1;
        let totalSlowQueryMetrics = 0;
        let totalProfilerMetrics = 0;

        // Exhaust the cursor by calling getMore until no more documents remain.
        while (cursorId && cursorId != 0) {
            const getMoreComment = `${comment}_getmore_${getMoreCounter}`;
            const getMoreResult = assert.commandWorked(
                db.runCommand({
                    getMore: cursorId,
                    collection: collName,
                    batchSize: 1,
                    comment: getMoreComment,
                }),
            );

            // Sum metrics from slow query logs for this getMore.
            const slowQueryLogs = getSlowQueryLogsByComment(db, getMoreComment, shardingTest, shouldShardCollection);
            const summedSlowQueryLogMetrics = sumExtensionMetricsFromSlowQueryLog(slowQueryLogs, "$metrics", "counter");
            totalSlowQueryMetrics += summedSlowQueryLogMetrics;

            // Sum metrics from profiler for this getMore if not running in a sharded environment.
            if (!shardingTest) {
                const profilerEntry = getGetMoreProfilerEntry(db, cursorId, getMoreComment);
                const extensionMetrics = getExtensionMetricsFromProfilerEntry(profilerEntry);
                totalProfilerMetrics += extensionMetrics.$metrics.counter;
            }

            cursorId = getMoreResult.cursor.id;
            getMoreCounter++;
        }

        // Validate that the total metrics from all getMore operations equals the expected sum.
        // The initial aggregate returned 1 document (counter: 1), so remaining documents should sum to 5 (2+3).
        const expectedRemainingSum = kCounterFieldSum - 1;
        assert.eq(
            totalSlowQueryMetrics,
            expectedRemainingSum,
            `Total slow query metrics from getMore operations should equal ${expectedRemainingSum}`,
        );

        if (!shardingTest) {
            assert.eq(
                totalProfilerMetrics,
                expectedRemainingSum,
                `Total profiler metrics from getMore operations should equal ${expectedRemainingSum}`,
            );
        }
    })();

    // Tests that a query with multiple instances of the same extension aggregate metrics together.
    (function multipleInstancesShareMetrics() {
        const comment = "multiple_instances_share_metrics_test";
        coll.aggregate([{$metrics: {}}, {$metrics: {}}], {comment});

        validateExtensionMetrics(db, collName, comment, kCounterFieldSum * 2, "$metrics", "counter", shardingTest);
    })();

    // Tests that a query with multiple extensions reports metrics from both extensions.
    (function multipleExtensionsCanReportMetricsInTheSameQuery() {
        const comment = "multiple_extensions_can_report_metrics_test";
        coll.aggregate([{$metrics: {}}, {$otherMetrics: {}}], {comment});

        validateExtensionMetrics(db, collName, comment, kCounterFieldSum, "$metrics", "counter", shardingTest);
        validateExtensionMetrics(db, collName, comment, 3, "$otherMetrics", "documentCount", shardingTest);
    })();
}

try {
    // Run tests against standalone mongod.
    const mongodConn = MongoRunner.runMongod(options);
    assert.neq(null, mongodConn, "mongod failed to start");
    runExtensionMetricsTests(mongodConn);
    MongoRunner.stopMongod(mongodConn);

    // Run tests against mongos in a sharded cluster (unsharded collection).
    const shardingTest = new ShardingTest({
        shards: 1,
        rs: {nodes: 1},
        mongos: 1,
        config: 1,
        mongosOptions: options,
        configOptions: options,
        rsOptions: options,
    });
    runExtensionMetricsTests(shardingTest.s, shardingTest);
    shardingTest.stop();

    // Run tests against mongos in a sharded cluster with a sharded collection.
    const shardingTestSharded = new ShardingTest({
        shards: 2,
        rs: {nodes: 1},
        mongos: 1,
        config: 1,
        mongosOptions: options,
        configOptions: options,
        rsOptions: options,
    });
    runExtensionMetricsTests(shardingTestSharded.s, shardingTestSharded, true);
    shardingTestSharded.stop();
} finally {
    deleteExtensionConfigs(extensionNames);
}
