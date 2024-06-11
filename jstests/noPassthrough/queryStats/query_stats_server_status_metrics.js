/**
 * Test the queryStats related serverStatus metrics.
 * @tags: [requires_fcv_72]
 */
"use strict";

function runTestWithMongodOptions(mongodOptions, test, testOptions) {
    const conn = MongoRunner.runMongod(mongodOptions);
    const testDB = conn.getDB('test');
    const coll = testDB[jsTestName()];

    test(conn, testDB, coll, testOptions);

    MongoRunner.stopMongod(conn);
}

// Helper to round up to the next highest power of 2 for our estimation.
function align(number) {
    return Math.pow(2, Math.ceil(Math.log2(number)));
}

/**
 * Returns the number of cores on this machine if it is known. Otherwise return 'undefined.'
 */
function numberOfCores(db) {
    const hostinfo = assert.commandWorked(db.hostInfo());
    if (hostinfo.os.type == "") {
        // We don't know the number of cores on this platform.
        return undefined;
    }
    return hostinfo.system.numCores;
}

function addApprox2MBOfStatsData(testDB, coll) {
    const k2MB = 2 * 1024 * 1024;

    const cmdObjTemplate = {
        find: coll.getName(),
        filter: {foo123: {$eq: "?"}},
    };

    const kEstimatedEntrySizeBytes = (() => {
        // Metrics stored per shape.
        const kNumCountersAndDates =
            4 /* top-level */ + (4 * 3) /* those with sum, min, max, sumOfSquares */;

        // Just a sample, will change based on where the test is run - shouldn't be off by too much
        // though.
        const kClientMetadataEst = {
            client: {application: {name: "MongoDB Shell"}},
            driver: {name: "MongoDB Internal Client", version: "7.1.0-alpha"},
            os: {type: "Linux", name: "Ubuntu", architecture: "aarch64", version: "22.04"}
        };

        const kCmdNsObj = {cmdNs: {db: testDB.getName(), coll: coll.getName()}};

        // Rough estimate of space needed for just the query stats 'Key' class members assuming
        // everything is 8 bytes.
        const kCxxOverhead = 96;

        // This is likely not to be exact - we are probably forgetting something. But we don't need
        // to be exact, just "good enough."
        return align(kNumCountersAndDates * 4 + Object.bsonsize(cmdObjTemplate) +
                     Object.bsonsize(kClientMetadataEst) + Object.bsonsize(kCmdNsObj) +
                     kCxxOverhead);
    })();
    const nIterations = k2MB / kEstimatedEntrySizeBytes;
    for (let i = 0; i <= nIterations; i++) {
        let newQuery = {["foo" + i]: "bar"};
        const cmdObj = cmdObjTemplate;
        cmdObj.filter = newQuery;
        const cmdRes = assert.commandWorked(testDB.runCommand(cmdObj));
        new DBCommandCursor(testDB, cmdRes).itcount();
    }
    // After inserting 'nIterations' new query shapes, at least 1/4 of them should have ended up in
    // the query stats store. We might have expected _all_ of them to end up in there, but we are
    // purposely trying to trigger eviction here, so we expect it not to have gotten all of them.
    assert.gte(4 * testDB.serverStatus().metrics.queryStats.numEntries,
               nIterations,
               testDB.serverStatus().metrics.queryStats);
}

/**
 * Tests that when the query stats store is reset to a size of 0, then the metrics should also go
 * back to 0.
 */
function assertCountersAreZeroAfterReset(conn, testDB) {
    assert.commandWorked(
        testDB.adminCommand({setParameter: 1, internalQueryStatsCacheSize: "0MB"}));
    const metricSnapshot = testDB.serverStatus().metrics.queryStats;
    assert.eq(0, metricSnapshot.numEntries, metricSnapshot);
    assert.eq(0, metricSnapshot.queryStatsStoreSizeEstimateBytes, metricSnapshot);
    assert.eq(0, metricSnapshot.maxSizeBytes, 0);
}

/**
 * Test serverStatus metric which counts the number of evicted entries.
 *
 * testOptions must include `resetCacheSize` bool field; e.g., { resetCacheSize : true }
 */
function evictionTest(conn, testDB, coll, testOptions) {
    const beforeMetrics = testDB.serverStatus().metrics.queryStats;
    assert.eq(beforeMetrics.numEvicted, 0);
    addApprox2MBOfStatsData(testDB, coll);
    if (!testOptions.resetCacheSize) {
        const afterMetrics = testDB.serverStatus().metrics.queryStats;
        const evictedAfter = afterMetrics.numEvicted;
        assert.gt(evictedAfter, 0, afterMetrics);
        return;
    }
    // Make sure number of evicted entries increases when the cache size is reset, which forces out
    // least recently used entries to meet the new, smaller size requirement.
    const midMetrics = testDB.serverStatus().metrics.queryStats;
    assert.eq(midMetrics.numEvicted, 0);
    assert.gt(midMetrics.numEntries, 0);
    assert.commandWorked(
        testDB.adminCommand({setParameter: 1, internalQueryStatsCacheSize: "1MB"}));
    const endMetrics = testDB.serverStatus().metrics.queryStats;
    assert.eq(endMetrics.maxSizeBytes, 1 * 1024 * 1024, endMetrics);
    assert.gt(endMetrics.numEvicted, 0, endMetrics);
    // Eviction should shrink the number of entries in the store.
    assert.gt(midMetrics.numEntries, endMetrics.numEntries, {mid: midMetrics, end: endMetrics});
    assertCountersAreZeroAfterReset(conn, testDB);
}

/**
 * Test serverStatus metric which counts the number of requests for which queryStats is not
 * collected due to rate-limiting.
 *
 * testOptions must include `samplingRate` and `numRequests` number fields;
 *  e.g., { samplingRate: -1, numRequests: 20 }
 */
function countRateLimitedRequestsTest(conn, testDB, coll, testOptions) {
    const numRateLimitedRequestsBefore =
        testDB.serverStatus().metrics.queryStats.numRateLimitedRequests;
    assert.eq(numRateLimitedRequestsBefore, 0);

    coll.insert({a: 0});

    // Running numRequests / 2 times since we dispatch two requests per iteration
    for (var i = 0; i < testOptions.numRequests / 2; i++) {
        coll.find({a: 0}).toArray();
        coll.aggregate([{$match: {a: 1}}]);
    }

    const numRateLimitedRequestsAfter =
        testDB.serverStatus().metrics.queryStats.numRateLimitedRequests;

    if (testOptions.samplingRate === 0) {
        // queryStats should not be collected for any requests.
        assert.eq(numRateLimitedRequestsAfter, testOptions.numRequests);
    } else if (testOptions.samplingRate >= testOptions.numRequests) {
        // queryStats should be collected for all requests.
        assert.eq(numRateLimitedRequestsAfter, 0);
    } else {
        // queryStats should be collected for some but not all requests.
        assert.gt(numRateLimitedRequestsAfter, 0);
        assert.lt(numRateLimitedRequestsAfter, testOptions.numRequests);
    }
}

function queryStatsStoreSizeEstimateTest(conn, testDB, coll, testOptions) {
    assert.eq(testDB.serverStatus().metrics.queryStats.queryStatsStoreSizeEstimateBytes, 0);
    let halfWayPointSize;
    // Only using three digit numbers (eg 100, 101) means the string length will be the same for all
    // entries and therefore the key size will be the same for all entries, which makes predicting
    // the total size of the store clean and easy.
    for (var i = 100; i < 200; i++) {
        coll.aggregate([{$match: {["foo" + i]: "bar"}}]).itcount();
        if (i == 150) {
            halfWayPointSize =
                testDB.serverStatus().metrics.queryStats.queryStatsStoreSizeEstimateBytes;
        }
    }
    // Confirm that queryStats store has grown and size is non-zero.
    assert.gt(halfWayPointSize, 0);
    const fullSize = testDB.serverStatus().metrics.queryStats.queryStatsStoreSizeEstimateBytes;
    assert.gt(fullSize, 0);
    // Make sure the final queryStats store size is twice as much as the halfway point size (+/- 5%)
    assert(fullSize >= halfWayPointSize * 1.95 && fullSize <= halfWayPointSize * 2.05,
           tojson({fullSize, halfWayPointSize}));
}

function queryStatsStoreWriteErrorsTest(conn, testDB, coll, testOptions) {
    const debugBuild = testDB.adminCommand('buildInfo').debug;
    if (debugBuild) {
        jsTestLog("Skipping queryStats store write errors test because debug build will tassert.");
        return;
    }

    const errorsBefore = testDB.serverStatus().metrics.queryStats.numQueryStatsStoreWriteErrors;
    assert.eq(errorsBefore, 0);
    for (let i = 0; i < 5; i++) {
        // Command should succeed and record the error.
        let query = {};
        query["foo" + i] = "bar";
        coll.aggregate([{$match: query}]).itcount();
    }

    // Make sure that we recorded a write error for each run.
    assert.eq(testDB.serverStatus().metrics.queryStats.numQueryStatsStoreWriteErrors, 5);
}

/**
 * Test that running the $queryStats aggregation stage correctly does or does not impact
 * serverStatus counters (whichever is applicable).
 */
function queryStatsAggregationStageTest(conn, testDB, coll) {
    // First, ensure that the query stats store is empty.
    assert.eq(testDB.serverStatus().metrics.queryStats.queryStatsStoreSizeEstimateBytes, 0);
    assert.eq(testDB.serverStatus().metrics.queryStats.numEvicted, 0);

    // Insert some query stats data and capture "before" serverStatus metrics.
    for (let i = 100; i < 200; i++) {
        coll.aggregate([{$match: {["foo" + i]: "bar"}}]).itcount();
    }

    let sizeBefore = testDB.serverStatus().metrics.queryStats.queryStatsStoreSizeEstimateBytes;
    const evictedBefore = testDB.serverStatus().metrics.queryStats.numEvicted;

    // Run a $queryStats pipeline. We should insert a new entry for this query.
    assert.commandWorked(
        testDB.adminCommand({aggregate: 1, pipeline: [{$queryStats: {}}], cursor: {}}));

    assert.eq(testDB.serverStatus().metrics.queryStats.numEvicted,
              evictedBefore,
              "$queryStats should not have triggered evictions");
    assert.gt(testDB.serverStatus().metrics.queryStats.queryStatsStoreSizeEstimateBytes,
              sizeBefore,
              "$queryStats pipeline should have been added to the query stats store");
    sizeBefore = testDB.serverStatus().metrics.queryStats.queryStatsStoreSizeEstimateBytes;

    // Now run $queryStats again. The command should be fully read-only.
    assert.commandWorked(
        testDB.adminCommand({aggregate: 1, pipeline: [{$queryStats: {}}], cursor: {}}));

    assert.eq(testDB.serverStatus().metrics.queryStats.numEvicted,
              evictedBefore,
              "$queryStats should not have triggered evictions");
    assert.eq(testDB.serverStatus().metrics.queryStats.queryStatsStoreSizeEstimateBytes,
              sizeBefore,
              "$queryStats pipeline should not have impacted query stats store size");
}

/**
 * Test the 'maxSizeBytes' and 'numPartitions' metrics.
 */
(function testMemorySizeAndNumPartitions() {
    let numPartitions = 0;
    runTestWithMongodOptions({setParameter: {internalQueryStatsCacheSize: "2MB"}},
                             (conn, testDB, coll) => {
                                 const metrics = testDB.serverStatus().metrics.queryStats;
                                 assert.eq(2 * 1024 * 1024, metrics.maxSizeBytes, metrics);
                                 numPartitions = metrics.numPartitions;
                                 assert.gt(numPartitions, 0, metrics);
                             });
    // Then increase the cache size to something much larger which should result in a greater number
    // of partitions. Since the number of partitions is in part based on the number of cores on the
    // machine, we will be somewhat relaxed with the assertion here.
    runTestWithMongodOptions(
        {setParameter: {internalQueryStatsCacheSize: "1GB"}}, (conn, testDB, coll) => {
            const metrics = testDB.serverStatus().metrics.queryStats;
            assert.eq(1 * 1024 * 1024 * 1024, metrics.maxSizeBytes, metrics);
            // We cap each partition at 16MB, so 1GB/16MB = 62.5 (rounded to 63) is the new expected
            // number of partitions. This is expected to be greater than what we started with.
            if (friendlyEqual(metrics.numPartitions, numPartitions)) {
                // What we started with was probably just equal to minimum number of partitions: the
                // number of cores on this machine. If for example this machine had 100 cores, both
                // cases would hit this minimum and we'd end up in this special case where they are
                // equal:
                assert.eq(numPartitions, numberOfCores(testDB));
            } else {
                assert.gt(metrics.numPartitions, numPartitions, metrics);
            }

            // Test that you can change it at runtime and have it reflected.

            assert.commandWorked(conn.getDB("admin").runCommand(
                {setParameter: 1, internalQueryStatsCacheSize: "8MB"}));
            const metricsPostGrowth = testDB.serverStatus().metrics.queryStats;
            const debugInfo = {original: metrics, postGrowth: metricsPostGrowth};
            assert.eq(8 * 1024 * 1024, metricsPostGrowth.maxSizeBytes, debugInfo);
            // You might expect the number of partitions to change based on that adjustment.
            // This won't happen until restart, since doing so at runtime would be a correctness and
            // performance challenge while the data structure is being accessed concurrently.
            assert.eq(metricsPostGrowth.numPartitions, metrics.numPartitions, debugInfo);
        });
})();

const noRateLimit = {
    internalQueryStatsRateLimit: -1
};
/**
 * In this configuration, we insert enough entries into the queryStats store to trigger LRU
 * eviction.
 */
runTestWithMongodOptions({setParameter: {...noRateLimit, internalQueryStatsCacheSize: "1MB"}},
                         evictionTest,
                         {resetCacheSize: false});
/**
 * In this configuration, eviction is triggered only when the queryStats store size is reset.
 *
 * Use an 8MB upper limit since our estimated size of the query stats entry is pretty rough and
 * meant to give us some wiggle room so we don't have to keep adjusting this test as we tweak it.
 */
runTestWithMongodOptions({setParameter: {...noRateLimit, internalQueryStatsCacheSize: "8MB"}},
                         evictionTest,
                         {resetCacheSize: true});

/**
 * In this configuration, every query is sampled, so no requests should be rate-limited.
 */
runTestWithMongodOptions({setParameter: noRateLimit},
                         countRateLimitedRequestsTest,
                         {samplingRate: 2147483647, numRequests: 20});

/**
 * In this configuration, the sampling rate is set so that some but not all requests are
 * rate-limited.
 */
runTestWithMongodOptions({setParameter: {internalQueryStatsRateLimit: 10}},
                         countRateLimitedRequestsTest,
                         {samplingRate: 10, numRequests: 20});

/**
 * Sample all queries and assert that the size of queryStats store is equal to num entries * entry
 * size
 */
runTestWithMongodOptions({setParameter: noRateLimit}, queryStatsStoreSizeEstimateTest);

/**
 * Use a very small queryStats store size and assert that errors in writing to the queryStats store
 * are tracked.
 */
runTestWithMongodOptions({setParameter: {...noRateLimit, internalQueryStatsCacheSize: "0.00001MB"}},
                         queryStatsStoreWriteErrorsTest);

/**
 * Tests that $queryStats has expected effects (or no effect) on counters.
 */
runTestWithMongodOptions({setParameter: {...noRateLimit, internalQueryStatsCacheSize: "2MB"}},
                         queryStatsAggregationStageTest);
