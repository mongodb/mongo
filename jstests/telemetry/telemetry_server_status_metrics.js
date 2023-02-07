/**
 * Test the telemetry related serverStatus metrics.
 */
load('jstests/libs/analyze_plan.js');
load("jstests/libs/feature_flag_util.js");

(function() {
"use strict";

if (!FeatureFlagUtil.isEnabled(db, "Telemetry")) {
    return;
}

function runTestWithMongodOptions(options, test, resetCacheSize) {
    const conn = MongoRunner.runMongod(options);
    const testDB = conn.getDB('test');
    const coll = testDB[jsTestName()];

    test(conn, testDB, coll, resetCacheSize);

    MongoRunner.stopMongod(conn);
}

// Test serverStatus metric which counts the number of evicted
// entries.
function evictionTest(conn, testDB, coll, resetCacheSize) {
    const evictedBefore = testDB.serverStatus().metrics.telemetry.numEvicted;
    assert.eq(evictedBefore, 0);
    for (var i = 0; i < 100; i++) {
        let query = {};
        for (var j = 0; j < 25; ++j) {
            query["foo.field.xyz." + i + "." + j] = 1;
            query["bar.field.xyz." + i + "." + j] = 2;
            query["baz.field.xyz." + i + "." + j] = 3;
        }
        coll.aggregate([{$match: query}]).itcount();
    }
    if (!resetCacheSize) {
        const evictedAfter = testDB.serverStatus().metrics.telemetry.numEvicted;
        assert.gt(evictedAfter, 0);
        return;
    }
    // Make sure number of evicted entries increases when the cache size is reset, which forces out
    // least recently used entries to meet the new, smaller size requirement.
    assert.eq(testDB.serverStatus().metrics.telemetry.numEvicted, 0);
    assert.commandWorked(
        testDB.adminCommand({setParameter: 1, internalQueryConfigureTelemetryCacheSize: "2MB"}));
    const evictedAfter = testDB.serverStatus().metrics.telemetry.numEvicted;
    assert.gt(evictedAfter, 0);
}

/**
 * In this configuration, every query is sampled. Each query has a key(~2200 - 2300 bytes) and
 * value(208 bytes). With a cache size of 3MB, there are 1024 partitions, each of max size ~ 3072
 * bytes. Each partition will only be able to fit one entry, eg a partition with one entry will be
 * considered full. When a second query shape/key falls into an already full partition, it will have
 * to evict the original entry.
 *
 * */
runTestWithMongodOptions({
    setParameter: {
        internalQueryConfigureTelemetryCacheSize: "3MB",
        internalQueryConfigureTelemetrySamplingRate: 2147483647
    },
},
                         evictionTest,
                         false);
/**
 * In this configuration, every query is sampled. Due to the large initial cache size, entries
 * should only be evicted once the cache is reset after telemetry metric collecting, is finished.
 * */
runTestWithMongodOptions({
    setParameter: {
        internalQueryConfigureTelemetryCacheSize: "10MB",
        internalQueryConfigureTelemetrySamplingRate: 2147483647
    },
},
                         evictionTest,
                         true);
}());