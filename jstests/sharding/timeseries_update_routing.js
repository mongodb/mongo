/**
 * Test routing of updates into sharded timeseries collection.
 *
 * @tags: [
 *   requires_fcv_50,
 *   requires_find_command,
 * ]
 */

(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");  // For 'TimeseriesTest' helpers.

const st = new ShardingTest({shards: 2, rs: {nodes: 2}});

//
// Constants used throughout all tests.
//

const dbName = 'testDB';
const collName = 'weather';
const bucketCollName = `system.buckets.${collName}`;
const bucketCollFullName = `${dbName}.${bucketCollName}`;

//
// Checks for feature flags.
//

if (!TimeseriesTest.timeseriesCollectionsEnabled(st.shard0)) {
    jsTestLog("Skipping test because the time-series collection feature flag is disabled");
    st.stop();
    return;
}

if (!TimeseriesTest.shardedtimeseriesCollectionsEnabled(st.shard0)) {
    jsTestLog("Skipping test because the sharded time-series collection feature flag is disabled");
    st.stop();
    return;
}

if (!TimeseriesTest.timeseriesUpdatesAndDeletesEnabled(st.shard0)) {
    jsTestLog(
        "Skipping test because the updates and deletes on time-series collection feature flag is disabled");
    st.stop();
    return;
}

if (!TimeseriesTest.shardedTimeseriesUpdatesAndDeletesEnabled(st.shard0)) {
    jsTestLog(
        "Skipping test because the updates and deletes on sharded time-series collection feature flag is disabled");
    st.stop();
    return;
}

const mongos = st.s;
const testDB = mongos.getDB(dbName);
const primary = st.shard0;
const primaryDB = primary.getDB(dbName);
const otherShard = st.shard1;
const otherShardDB = otherShard.getDB(dbName);

testDB.dropDatabase();
assert.commandWorked(mongos.adminCommand({enableSharding: dbName}));
st.ensurePrimaryShard(testDB.getName(), primary.shardName);

assert.commandWorked(testDB.createCollection(
    collName, {timeseries: {timeField: "time", metaField: "location", granularity: "hours"}}));

const testColl = testDB[collName];

//
// Helper functions
//

function testUpdateRouting({updates, nModified, shardsTargetedCount}) {
    // Restart profiling.
    for (const db of [primaryDB, otherShardDB]) {
        db.setProfilingLevel(0);
        db.system.profile.drop();
        db.setProfilingLevel(2);
    }

    // Verify output documents.
    const updateCommand = {update: testColl.getName(), updates};
    const result = assert.commandWorked(testDB.runCommand(updateCommand));

    assert.eq(nModified, result.nModified);

    // Verify profiling output.
    if (shardsTargetedCount > 0) {
        let filter = {"op": "update", "ns": "testDB.weather"};
        let actualCount = 0;
        for (const db of [primaryDB, otherShardDB]) {
            const expectedEntries = db.system.profile.find(filter).toArray();
            actualCount += expectedEntries.length;
        }
        assert.eq(actualCount, shardsTargetedCount);
    }
}

(function setUpTestColl() {
    assert.commandWorked(testDB.adminCommand(
        {shardCollection: testColl.getFullName(), key: {"location.city": 1, time: 1}}));

    const data = [
        // Cork.
        {
            location: {city: "Cork", coordinates: [-12, 10]},
            time: ISODate("2021-05-18T08:00:00.000Z"),
            temperature: 12,
        },
        {
            location: {city: "Cork", coordinates: [0, 0]},
            time: ISODate("2021-05-18T07:30:00.000Z"),
            temperature: 15,
        },
        // Dublin.
        {
            location: {city: "Dublin", coordinates: [25, -43]},
            time: ISODate("2021-05-18T08:00:00.000Z"),
            temperature: 12,
        },
        {
            location: {city: "Dublin", coordinates: [0, 0]},
            time: ISODate("2021-05-18T08:00:00.000Z"),
            temperature: 22,
        },
        {
            location: {city: "Dublin", coordinates: [25, -43]},
            time: ISODate("2021-05-18T08:30:00.000Z"),
            temperature: 12.5,
        },
        {
            location: {city: "Dublin", coordinates: [25, -43]},
            time: ISODate("2021-05-18T09:00:00.000Z"),
            temperature: 13,
        },
        // Galway.
        {
            location: {city: "Galway", coordinates: [22, 44]},
            time: ISODate("2021-05-18T08:00:00.000Z"),
            temperature: 20,
        },
        {
            location: {city: "Galway", coordinates: [0, 0]},
            time: ISODate("2021-05-18T09:00:00.000Z"),
            temperature: 20,
        },
    ];
    assert.commandWorked(testColl.insertMany(data));
})();

(function defineChunks() {
    function splitAndMove(city, minTime, destination) {
        assert.commandWorked(st.s.adminCommand(
            {split: bucketCollFullName, middle: {"meta.city": city, 'control.min.time': minTime}}));
        assert.commandWorked(st.s.adminCommand({
            movechunk: bucketCollFullName,
            find: {"meta.city": city, 'control.min.time': minTime},
            to: destination.shardName,
            _waitForDelete: true
        }));
    }

    // Place the Dublin buckets on the primary and split the other buckets across both shards.
    splitAndMove("Galway", ISODate("2021-05-18T08:00:00.000Z"), otherShard);
    splitAndMove("Dublin", MinKey, primary);
    splitAndMove("Cork", ISODate("2021-05-18T09:00:00.000Z"), otherShard);
})();

// All Dublin documents exist on the primary, so we should only target the one shard.
testUpdateRouting({
    updates: [{
        q: {"location.city": "Dublin"},
        u: {$set: {"location.coordinates": [123, -123]}},
        multi: true,
    }],
    nModified: 4,
    shardsTargetedCount: 1
});

// Galway documents exist on both shards, so we should target both.
testUpdateRouting({
    updates: [{
        q: {"location.city": "Galway"},
        u: {$set: {"location.coordinates": [6, 7]}},
        multi: true,
    }],
    nModified: 2,
    shardsTargetedCount: 2
});

// All shards need to be targeted for an empty query.
testUpdateRouting({
    updates: [{
        q: {},
        u: {$set: {"location.coordinates": [222, 111]}},
        multi: true,
    }],
    nModified: 8,
    shardsTargetedCount: 2
});

st.stop();
})();
