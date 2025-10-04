/**
 * Tests 'deleteOne' command on sharded collections without specifying shard key.
 *
 * @tags: [
 * ]
 */

import {getTimeseriesCollForDDLOps} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";
import {getTimeseriesCollForRawOps} from "jstests/libs/raw_operation_utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 2, rs: {nodes: 2}});

//
// Constants used throughout all tests.
//
const dbName = "testDB";
const collNameWithMeta = "weatherWithLocationMeta";
const collNameWithoutMeta = "weather";
const mongos = st.s;
const testDB = mongos.getDB(dbName);
const primary = st.shard0;
const primaryDB = primary.getDB(dbName);
const otherShard = st.shard1;
const otherDB = otherShard.getDB(dbName);

assert.commandWorked(mongos.adminCommand({enableSharding: dbName, primaryShard: primary.shardName}));

const shard0RoutingValues = {
    shardNumber: 0,
    timestamp: ISODate("2000-05-18T08:00:00.000Z"),
};
const shard1RoutingValues = {
    shardNumber: 1,
    timestamp1: ISODate("2010-05-18T08:00:00.000Z"),
    timestamp2: ISODate("2010-05-19T08:00:00.000Z"),
};

const data = [
    // Cork.
    {
        location: {city: "Cork", shardNumber: shard0RoutingValues.shardNumber},
        time: shard0RoutingValues.timestamp,
        temperature: 12,
    },
    {
        location: {city: "Cork", shardNumber: shard0RoutingValues.shardNumber},
        time: shard0RoutingValues.timestamp,
        temperature: 15,
    },
    // Dublin.
    {
        location: {city: "Dublin", shardNumber: shard0RoutingValues.shardNumber},
        time: shard0RoutingValues.timestamp,
        temperature: 12,
    },
    {
        location: {city: "Dublin", shardNumber: shard0RoutingValues.shardNumber},
        time: shard0RoutingValues.timestamp,
        temperature: 22,
    },
    {
        location: {city: "Dublin", shardNumber: shard1RoutingValues.shardNumber},
        time: shard1RoutingValues.timestamp1,
        temperature: 12.5,
    },
    {
        location: {city: "Dublin", shardNumber: shard1RoutingValues.shardNumber},
        time: shard1RoutingValues.timestamp1,
        temperature: 13,
    },
    // Galway.
    {
        location: {city: "Galway", shardNumber: shard1RoutingValues.shardNumber},
        time: shard1RoutingValues.timestamp1,
        temperature: 20,
    },
    {
        location: {city: "Galway", shardNumber: shard1RoutingValues.shardNumber},
        time: shard1RoutingValues.timestamp1,
        temperature: 20,
    },
    // New York City.
    {
        _id: 0,
        location: {city: "New York City", shardNumber: shard0RoutingValues.shardNumber},
        time: shard0RoutingValues.timestamp,
        temperature: 20,
    },
    {
        location: {city: "New York City", shardNumber: shard1RoutingValues.shardNumber},
        time: shard1RoutingValues.timestamp2,
        temperature: 39,
    },
    {
        _id: 100,
        location: {city: "New York City", shardNumber: shard1RoutingValues.shardNumber},
        time: shard1RoutingValues.timestamp2,
        temperature: 20,
    },
];

// Set up a sharded time-series collection and split up the data points across 2 shards.
const setUpShardedTimeseriesCollection = function (collName) {
    const testColl = testDB[collName];
    let splitObject = null;

    if (collName == collNameWithoutMeta) {
        assert.commandWorked(
            testDB.createCollection(collName, {timeseries: {timeField: "time", granularity: "hours"}}),
        );
        assert.commandWorked(testDB.adminCommand({shardCollection: testColl.getFullName(), key: {"time": 1}}));

        // In the measurements used for this test, timestamps use either year 2000 or year 2010 so
        // this split key in the year 2005 splits measurements across 2 shards accordingly.
        splitObject = {"control.min.time": ISODate("2005-05-18T08:00:00.000Z")};
    } else {
        assert.commandWorked(
            testDB.createCollection(collName, {
                timeseries: {timeField: "time", metaField: "location", granularity: "hours"},
            }),
        );
        assert.commandWorked(
            testDB.adminCommand({shardCollection: testColl.getFullName(), key: {"location.shardNumber": 1}}),
        );
        splitObject = {"meta.shardNumber": 1};
    }

    assert.commandWorked(testColl.insertMany(data, {ordered: false}));

    // Shard 0 :   2 Corks   |   2 Dublins   |   1 New York City
    // Shard 1 :   2 Dublins |   2 Galways   |   2 New York Citys
    assert.commandWorked(
        st.s.adminCommand({split: getTimeseriesCollForDDLOps(testDB, testColl).getFullName(), middle: splitObject}),
    );

    // Move chunks to the other shard.
    assert.commandWorked(
        st.s.adminCommand({
            movechunk: getTimeseriesCollForDDLOps(testDB, testColl).getFullName(),
            find: splitObject,
            to: otherShard.shardName,
            _waitForDelete: true,
        }),
    );

    // Ensures that each shard owns one chunk.
    const counts = st.chunkCounts(collName, dbName);
    assert.eq(1, counts[primary.shardName], counts);
    assert.eq(1, counts[otherShard.shardName], counts);

    // Ensure there is at least one bucket document on each shard.
    const numBucketDocsOnPrimaryDB = primaryDB[getTimeseriesCollForRawOps(testDB, collName)]
        .find({})
        .rawData()
        .toArray();
    const numBucketDocsOnOtherDB = otherDB[getTimeseriesCollForRawOps(testDB, collName)].find({}).rawData().toArray();
    assert.gt(
        numBucketDocsOnPrimaryDB.length,
        1,
        `Bucket documents on primaryDB: ${tojson(numBucketDocsOnPrimaryDB)} Bucket documents on otherDB: ${tojson(
            numBucketDocsOnOtherDB,
        )}`,
    );
    assert.gt(
        numBucketDocsOnOtherDB.length,
        1,
        `Bucket documents on primaryDB: ${tojson(numBucketDocsOnPrimaryDB)} Bucket documents on otherDB: ${tojson(
            numBucketDocsOnOtherDB,
        )}`,
    );
};

let numOfDeletedMeasurements = 0;
const runDeleteOneWithQuery = function (collName, query, expectedN) {
    jsTestLog(`running deleteOne() with query: ${tojson(query)} on collection: "${collName}"`);
    const deleteCommand = {
        delete: collName,
        deletes: [
            {
                q: query,
                limit: 1,
            },
        ],
    };
    const result = assert.commandWorked(testDB.runCommand(deleteCommand));

    assert.eq(expectedN, result.n);
    numOfDeletedMeasurements += expectedN;
};

const runTests = function (collName) {
    numOfDeletedMeasurements = 0;

    // Set up sharded time-series collections with data split across 2 shards.
    setUpShardedTimeseriesCollection(collName);

    // We expect 'deleteOne' to succeed without specifying a shard key in the delete query.
    runDeleteOneWithQuery(collName, {"location.city": "Galway"}, 1);
    runDeleteOneWithQuery(collName, {"location.city": "Dublin"}, 1);
    runDeleteOneWithQuery(collName, {"location.city": "Galway"}, 1);

    // We expect 'deleteOne' on non-existent measurements to succeed as no-ops.
    runDeleteOneWithQuery(collName, {"location.city": "Chicago"}, 0);
    runDeleteOneWithQuery(collName, {"location.city": "Galway"}, 0);

    // We expect 'deleteOne' on the time field to succeed.
    runDeleteOneWithQuery(collName, {"time": shard0RoutingValues.timestamp}, 1);
    runDeleteOneWithQuery(collName, {"time": shard1RoutingValues.timestamp1}, 1);

    // We expect 'deleteOne' on the _id field to succeed.
    runDeleteOneWithQuery(collName, {"_id": 100}, 1);

    // We expect 'deleteOne' on metric fields to succeed.
    runDeleteOneWithQuery(collName, {"temperature": 39}, 1);

    // We expect 'deleteOne' with an empty predicate to succeed.
    runDeleteOneWithQuery(collName, {}, 1);

    // Verify that the expected number of measurements exist.
    const originalCount = data.length;
    const remainingMeasurements = testDB[collName].find({}).toArray();
    assert.eq(
        originalCount - numOfDeletedMeasurements,
        remainingMeasurements.length,
        "Remaining Measurements: " + tojson(remainingMeasurements),
    );
};

// Run tests on a collection with a metaField specified upon creation.
runTests(collNameWithMeta);
// Run tests on a collection with no metaField specified.
runTests(collNameWithoutMeta);

st.stop();
