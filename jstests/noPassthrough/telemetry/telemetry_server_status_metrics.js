/**
 * Test the telemetry related serverStatus metrics.
 * @tags: [featureFlagTelemetry]
 */
load('jstests/libs/analyze_plan.js');

(function() {
"use strict";

function runTestWithMongodOptions(mongodOptions, test, testOptions) {
    const conn = MongoRunner.runMongod(mongodOptions);
    const testDB = conn.getDB('test');
    const coll = testDB[jsTestName()];

    test(conn, testDB, coll, testOptions);

    MongoRunner.stopMongod(conn);
}

/**
 * Test serverStatus metric which counts the number of evicted entries.
 *
 * testOptions must include `resetCacheSize` bool field; e.g., { resetCacheSize : true }
 */
function evictionTest(conn, testDB, coll, testOptions) {
    const evictedBefore = testDB.serverStatus().metrics.telemetry.numEvicted;
    assert.eq(evictedBefore, 0);
    for (var i = 0; i < 4000; i++) {
        let query = {};
        query["foo" + i] = "bar";
        coll.aggregate([{$match: query}]).itcount();
    }
    if (!testOptions.resetCacheSize) {
        const evictedAfter = testDB.serverStatus().metrics.telemetry.numEvicted;
        assert.gt(evictedAfter, 0);
        return;
    }
    // Make sure number of evicted entries increases when the cache size is reset, which forces out
    // least recently used entries to meet the new, smaller size requirement.
    assert.eq(testDB.serverStatus().metrics.telemetry.numEvicted, 0);
    assert.commandWorked(
        testDB.adminCommand({setParameter: 1, internalQueryConfigureTelemetryCacheSize: "1MB"}));
    const evictedAfter = testDB.serverStatus().metrics.telemetry.numEvicted;
    assert.gt(evictedAfter, 0);
}

/**
 * Test serverStatus metric which counts the number of requests for which telemetry is not collected
 * due to rate-limiting.
 *
 * testOptions must include `samplingRate` and `numRequests` number fields;
 *  e.g., { samplingRate: 2147483647, numRequests: 20 }
 */
function countRateLimitedRequestsTest(conn, testDB, coll, testOptions) {
    const numRateLimitedRequestsBefore =
        testDB.serverStatus().metrics.telemetry.numRateLimitedRequests;
    assert.eq(numRateLimitedRequestsBefore, 0);

    coll.insert({a: 0});

    // Running numRequests / 2 times since we dispatch two requests per iteration
    for (var i = 0; i < testOptions.numRequests / 2; i++) {
        coll.find({a: 0}).toArray();
        coll.aggregate([{$match: {a: 1}}]);
    }

    const numRateLimitedRequestsAfter =
        testDB.serverStatus().metrics.telemetry.numRateLimitedRequests;

    if (testOptions.samplingRate === 0) {
        // Telemetry should not be collected for any requests.
        assert.eq(numRateLimitedRequestsAfter, testOptions.numRequests);
    } else if (testOptions.samplingRate >= testOptions.numRequests) {
        // Telemetry should be collected for all requests.
        assert.eq(numRateLimitedRequestsAfter, 0);
    } else {
        // Telemetry should be collected for some but not all requests.
        assert.gt(numRateLimitedRequestsAfter, 0);
        assert.lt(numRateLimitedRequestsAfter, testOptions.numRequests);
    }
}

function telemetryStoreSizeEstimateTest(conn, testDB, coll, testOptions) {
    assert.eq(testDB.serverStatus().metrics.telemetry.telemetryStoreSizeEstimateBytes, 0);
    let halfWayPointSize;
    // Only using three digit numbers (eg 100, 101) means the string length will be the same for all
    // entries and therefore the key size will be the same for all entries, which makes predicting
    // the total size of the store clean and easy.
    for (var i = 100; i < 200; i++) {
        coll.aggregate([{$match: {["foo" + i]: "bar"}}]).itcount();
        if (i == 150) {
            halfWayPointSize =
                testDB.serverStatus().metrics.telemetry.telemetryStoreSizeEstimateBytes;
        }
    }
    // Confirm that telemetry store has grown and size is non-zero.
    assert.gt(halfWayPointSize, 0);
    const fullSize = testDB.serverStatus().metrics.telemetry.telemetryStoreSizeEstimateBytes;
    assert.gt(fullSize, 0);
    // Make sure the final telemetry store size is twice as much as the halfway point size (+/- 5%)
    assert(fullSize >= halfWayPointSize * 1.95 && fullSize <= halfWayPointSize * 2.05,
           tojson({fullSize, halfWayPointSize}));
}

function telemetryStoreWriteErrorsTest(conn, testDB, coll, testOptions) {
    const debugBuild = testDB.adminCommand('buildInfo').debug;
    if (debugBuild) {
        jsTestLog("Skipping telemetry store write errors test because debug build will tassert.");
        return;
    }

    const errorsBefore = testDB.serverStatus().metrics.telemetry.numTelemetryStoreWriteErrors;
    assert.eq(errorsBefore, 0);
    for (let i = 0; i < 5; i++) {
        // Command should succeed and record the error.
        let query = {};
        query["foo" + i] = "bar";
        coll.aggregate([{$match: query}]).itcount();
    }

    // Make sure that we recorded a write error for each run.
    // TODO SERVER-73152 we attempt to write to the telemetry store twice for each aggregate, which
    // seems wrong.
    assert.eq(testDB.serverStatus().metrics.telemetry.numTelemetryStoreWriteErrors, 10);
}

/**
 * In this configuration, we insert enough entries into the telemetry store to trigger LRU
 * eviction.
 */
runTestWithMongodOptions({
    setParameter: {
        internalQueryConfigureTelemetryCacheSize: "1MB",
        internalQueryConfigureTelemetrySamplingRate: -1
    },
},
                         evictionTest,
                         {resetCacheSize: false});
/**
 * In this configuration, eviction is triggered only when the telemetry store size is reset.
 * */
runTestWithMongodOptions({
    setParameter: {
        internalQueryConfigureTelemetryCacheSize: "4MB",
        internalQueryConfigureTelemetrySamplingRate: -1
    },
},
                         evictionTest,
                         {resetCacheSize: true});

/**
 * In this configuration, every query is sampled, so no requests should be rate-limited.
 */
runTestWithMongodOptions({
    setParameter: {internalQueryConfigureTelemetrySamplingRate: -1},
},
                         countRateLimitedRequestsTest,
                         {samplingRate: 2147483647, numRequests: 20});

/**
 * In this configuration, the sampling rate is set so that some but not all requests are
 * rate-limited.
 */
runTestWithMongodOptions({
    setParameter: {internalQueryConfigureTelemetrySamplingRate: 10},
},
                         countRateLimitedRequestsTest,
                         {samplingRate: 10, numRequests: 20});

/**
 * Sample all queries and assert that the size of telemetry store is equal to num entries * entry
 * size
 */
runTestWithMongodOptions({
    setParameter: {internalQueryConfigureTelemetrySamplingRate: -1},
},
                         telemetryStoreSizeEstimateTest);

/**
 * Use a very small telemetry store size and assert that errors in writing to the telemetry store
 * are tracked.
 */
runTestWithMongodOptions({
    setParameter: {
        internalQueryConfigureTelemetryCacheSize: "0.00001MB",
        internalQueryConfigureTelemetrySamplingRate: -1
    },
},
                         telemetryStoreWriteErrorsTest);
}());
