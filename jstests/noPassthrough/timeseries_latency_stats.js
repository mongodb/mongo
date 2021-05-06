/**
 * Tests inserting sample data into the time-series buckets collection.
 * This test is for the simple case of only one measurement per bucket.
 * @tags: [
 *     assumes_no_implicit_collection_creation_after_drop,
 *     does_not_support_stepdowns,
 *     requires_fcv_49,
 *     requires_find_command,
 *     requires_getmore,
 * ]
 */
(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");

const conn = MongoRunner.runMongod();

if (!TimeseriesTest.timeseriesCollectionsEnabled(conn)) {
    jsTestLog("Skipping test because the time-series collection feature flag is disabled");
    MongoRunner.stopMongod(conn);
    return;
}

const testDB = conn.getDB('test');
const coll = testDB.timeseries_latency_stats;
const bucketsColl = testDB.getCollection('system.buckets.' + coll.getName());

coll.drop();

const timeFieldName = 'time';
assert.commandWorked(
    testDB.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName}}));
assert.contains(bucketsColl.getName(), testDB.getCollectionNames());

const getLatencyStats = () => {
    const stats = coll.aggregate([{$collStats: {latencyStats: {}}}]).next();
    assert(stats.hasOwnProperty("latencyStats"));
    assert(stats.latencyStats.hasOwnProperty("writes"));
    return stats.latencyStats.writes;
};

const stats1 = getLatencyStats();
assert.eq(stats1.ops, 0);
assert.eq(stats1.latency, 0);

assert.commandWorked(coll.insert({[timeFieldName]: new Date(), x: 1}));

const stats2 = getLatencyStats();
assert.eq(stats2.ops, 1);
assert.gt(stats2.latency, stats1.latency);

const reps = 10;
for (let i = 0; i < reps; ++i) {
    assert.commandWorked(coll.insert({[timeFieldName]: new Date(), x: 1}));
}

const stats3 = getLatencyStats();
assert.eq(stats3.ops, 1 + reps);
assert.gt(stats3.latency, stats2.latency);

MongoRunner.stopMongod(conn);
})();
