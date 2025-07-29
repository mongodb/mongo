/**
 * Test the queryStats related serverStatus metrics.
 * @tags: [requires_fcv_72]
 */
(function() {
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
}
/**
 * Test serverStatus metric which counts the number of evicted entries.
 *
 * testOptions must include `resetCacheSize` bool field; e.g., { resetCacheSize : true }
 */
function evictionTest(conn, testDB, coll, testOptions) {
    const evictedBefore = testDB.serverStatus().metrics.queryStats.numEvicted;
    assert.eq(evictedBefore, 0);
    addApprox2MBOfStatsData(testDB, coll);
    if (!testOptions.resetCacheSize) {
        const evictedAfter = testDB.serverStatus().metrics.queryStats.numEvicted;
        assert.gt(evictedAfter, 0, testDB.serverStatus().metrics.queryStats);
        return;
    }
    // Make sure number of evicted entries increases when the cache size is reset, which forces out
    // least recently used entries to meet the new, smaller size requirement.
    assert.eq(testDB.serverStatus().metrics.queryStats.numEvicted, 0);
    assert.commandWorked(
        testDB.adminCommand({setParameter: 1, internalQueryStatsCacheSize: "1MB"}));
    const evictedAfter = testDB.serverStatus().metrics.queryStats.numEvicted;
    assert.gt(evictedAfter, 0);
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
    const startingSize = testDB.serverStatus().metrics.queryStats.queryStatsStoreSizeEstimateBytes;
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
    assert.gt(halfWayPointSize, startingSize);
    let fullSize = testDB.serverStatus().metrics.queryStats.queryStatsStoreSizeEstimateBytes;
    assert.gt(fullSize, 0);
    // Make sure the final queryStats store size is twice as much as the halfway point size (+/-
    // 5%). First subtract starting size so that internal queries don't skew our comparisons.
    halfWayPointSize -= startingSize;
    fullSize -= startingSize;
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
    for (let i = 0; i < 5; i++) {
        // Command should succeed and record the error.
        let query = {};
        query["foo" + i] = "bar";
        coll.aggregate([{$match: query}]).itcount();
    }

    // Make sure that we recorded a write error for each run.
    assert.eq(testDB.serverStatus().metrics.queryStats.numQueryStatsStoreWriteErrors,
              errorsBefore + 5);
}

/**
 * Test that running the $queryStats aggregation stage correctly does or does not impact
 * serverStatus counters (whichever is applicable).
 */
function queryStatsAggregationStageTest(conn, testDB, coll) {
    assert.gte(testDB.serverStatus().metrics.queryStats.queryStatsStoreSizeEstimateBytes, 0);
    assert.eq(testDB.serverStatus().metrics.queryStats.numEvicted, 0);

    // Insert some query stats data and capture "before" serverStatus metrics. It doesn't matter how
    // many we insert as long as it fits in one batch (so that the $queryStats cursor is exhausted
    // on the first call).
    for (let i = 100; i < 150; i++) {
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
 * In this configuration, we insert enough entries into the queryStats store to trigger LRU
 * eviction.
 */
runTestWithMongodOptions({
    setParameter: {internalQueryStatsCacheSize: "1MB", internalQueryStatsRateLimit: -1},
},
                         evictionTest,
                         {resetCacheSize: false});
/**
 * In this configuration, eviction is triggered only when the queryStats store size is reset.
 *
 * Use an 8MB upper limit since our estimated size of the query stats entry is pretty rough and
 * meant to give us some wiggle room so we don't have to keep adjusting this test as we tweak it.
 */
runTestWithMongodOptions({
    setParameter: {internalQueryStatsCacheSize: "8MB", internalQueryStatsRateLimit: -1},
},
                         evictionTest,
                         {resetCacheSize: true});

/**
 * In this configuration, every query is sampled, so no requests should be rate-limited.
 */
runTestWithMongodOptions({
    setParameter: {internalQueryStatsRateLimit: -1},
},
                         countRateLimitedRequestsTest,
                         {samplingRate: 2147483647, numRequests: 20});

/**
 * In this configuration, the sampling rate is set so that some but not all requests are
 * rate-limited.
 */
runTestWithMongodOptions({
    setParameter: {internalQueryStatsRateLimit: 10},
},
                         countRateLimitedRequestsTest,
                         {samplingRate: 10, numRequests: 20});

/**
 * Sample all queries and assert that the size of queryStats store is equal to num entries * entry
 * size
 */
runTestWithMongodOptions({
    setParameter: {internalQueryStatsRateLimit: -1},
},
                         queryStatsStoreSizeEstimateTest);

/**
 * Use a very small queryStats store size and assert that errors in writing to the queryStats store
 * are tracked.
 */
runTestWithMongodOptions({
    setParameter: {internalQueryStatsCacheSize: "0.00001MB", internalQueryStatsRateLimit: -1},
},
                         queryStatsStoreWriteErrorsTest);

/**
 * Tests that $queryStats has expected effects (or no effect) on counters.
 */
runTestWithMongodOptions({
    setParameter: {internalQueryStatsCacheSize: "2MB", internalQueryStatsRateLimit: -1},
},
                         queryStatsAggregationStageTest);
}());
