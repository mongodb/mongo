/**
 * Tests 'deleteOne' command on sharded collections when specifying shard key.
 *
 * @tags: [
 *   # To avoid multiversion tests
 *   requires_fcv_70,
 *   # To avoid burn-in tests in in-memory build variants
 *   requires_persistence,
 *   featureFlagTimeseriesDeletesSupport,
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

const data = [
    // Cork.
    {
        location: {city: "Cork", shardNumber: 0},
        time: ISODate("2021-05-18T08:00:00.000Z"),
        temperature: 12,
    },
    {
        location: {city: "Cork", shardNumber: 0},
        time: ISODate("2021-05-18T07:30:00.000Z"),
        temperature: 15,
    },
    // Dublin.
    {
        location: {city: "Dublin", shardNumber: 0},
        time: ISODate("2021-05-18T08:00:00.000Z"),
        temperature: 12,
    },
    {
        location: {city: "Dublin", shardNumber: 0},
        time: ISODate("2021-05-18T08:00:00.000Z"),
        temperature: 22,
    },
    {
        location: {city: "Dublin", shardNumber: 1},
        time: ISODate("2021-05-18T08:30:00.000Z"),
        temperature: 12.5,
    },
    {
        location: {city: "Dublin", shardNumber: 1},
        time: ISODate("2021-05-18T09:00:00.000Z"),
        temperature: 13,
    },
    // Galway.
    {
        location: {city: "Galway", shardNumber: 1},
        time: ISODate("2021-05-18T08:00:00.000Z"),
        temperature: 20,
    },
    {
        location: {city: "Galway", shardNumber: 1},
        time: ISODate("2021-05-18T09:00:00.000Z"),
        temperature: 20,
    },
];

// Set up a sharded time-series collection and split up the data points across 2 shards.
{
    assert.commandWorked(testDB.adminCommand(
        {shardCollection: testColl.getFullName(), key: {"location.shardNumber": 1}}));
    assert.commandWorked(testColl.insertMany(data, {ordered: false}));

    // Shard 0 : 2 Corks, 2 Dublins
    // Shard 1 : 2 Dublins, 2 Galways
    assert.commandWorked(
        st.s.adminCommand({split: bucketCollFullName, middle: {"meta.shardNumber": 1}}));

    // Move chunks to the other shard.
    assert.commandWorked(st.s.adminCommand({
        movechunk: bucketCollFullName,
        find: {"meta.shardNumber": 1},
        to: otherShard.shardName,
        _waitForDelete: true
    }));

    // Ensures that each shard owns one chunk.
    const counts = st.chunkCounts(bucketCollName, dbName);
    assert.eq(1, counts[primary.shardName], counts);
    assert.eq(1, counts[otherShard.shardName], counts);
}

let numOfDeletedMeasurements = 0;
const runDeleteOneWithShardKey = function(query, expectedN = 1) {
    jsTestLog(`Expecting ${expectedN} deletion(s) for 'deleteOne' with query = ${tojson(query)}`);
    const deleteCommand = {
        delete: testColl.getName(),
        deletes: [{
            q: query,
            limit: 1,
        }]
    };
    const result = assert.commandWorked(testDB.runCommand(deleteCommand));

    assert.eq(expectedN, result.n);
    numOfDeletedMeasurements += expectedN;
};

const originalCount = data.length;

// Expect deleteOne with the meta field to succeed.
runDeleteOneWithShardKey({"location.shardNumber": 0}, 1);

// Expect deleteOne with the metaField and 'city' field to succeed.
runDeleteOneWithShardKey({"location.shardNumber": 0, "location.city": "Cork"}, 1);

// Expect deleteOne with the metaField and 'city' field to succeed.
runDeleteOneWithShardKey({"location.shardNumber": 1, "location.city": "Dublin"}, 1);

// Expect no documents to be deleted when there's no document represented by the query.
runDeleteOneWithShardKey({"location.shardNumber": 1, "location.city": "Cork"}, 0);

// Verify the expected number of documents exist.
const remainingDocuments = testColl.find({}).toArray();
assert.eq(originalCount - numOfDeletedMeasurements,
          remainingDocuments.length,
          "Remaining Documents: " + tojson(remainingDocuments));

st.stop();
})();
