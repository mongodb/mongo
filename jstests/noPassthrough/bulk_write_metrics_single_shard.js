/**
 * Test that bulkWrite emit the same metrics that corresponding calls to update, delete and insert.
 *
 * @tags: [requires_fcv_80, featureFlagTimeseriesUpdatesSupport]
 */

import {BulkWriteMetricChecker} from "jstests/libs/bulk_write_utils.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

function runTest(isMongos, cluster, bulkWrite, retryCount, timeseries) {
    // We are ok with the randomness here since we clearly log the state.
    const errorsOnly = Math.random() < 0.5;
    print(`Running on a ${isMongos ? "ShardingTest" : "ReplSetTest"} with bulkWrite = ${
        bulkWrite}, errorsOnly = ${errorsOnly} and timeseries = ${timeseries}.`);

    const dbName = "testDB";
    const collName = "testColl";
    const namespace = `${dbName}.${collName}`;
    const session = isMongos ? cluster.s.startSession() : cluster.getPrimary().startSession();
    const testDB = session.getDatabase(dbName);

    if (timeseries) {
        assert.commandWorked(testDB.createCollection(collName, {
            timeseries: {timeField: "timestamp", bucketMaxSpanSeconds: 1, bucketRoundingSeconds: 1}
        }));
    }

    if (isMongos) {
        assert.commandWorked(testDB.adminCommand({'enableSharding': dbName}));

        assert.commandWorked(
            testDB.adminCommand({shardCollection: namespace, key: {timestamp: 1}}));
    }

    const coll = testDB[collName];

    const metricChecker =
        new BulkWriteMetricChecker(testDB,
                                   [namespace],
                                   bulkWrite,
                                   isMongos,
                                   false /*fle*/,
                                   errorsOnly,
                                   retryCount,
                                   timeseries,
                                   ISODate("2021-05-18T00:00:00.000Z") /* defaultTimestamp */);

    const key = ISODate("2024-01-30T00:00:00.00Z");
    metricChecker.executeCommand({insert: collName, documents: [{_id: 1, timestamp: key}]});

    metricChecker.checkMetricsWithRetries(
        "Update with retry and the shard key in the filter",
        [{update: 0, filter: {timestamp: key}, updateMods: {$set: {x: 4}}}],
        {update: collName, updates: [{q: {timestamp: key}, u: {$set: {x: 4}}}]},
        {
            updated: 1,
            updateCount: 1,
            updateShardField: "oneShard",
            opcounterFactor:
                !isMongos || !timeseries ? 1 : 2  // TODO SERVER-85757 remove opcounterFactor.
        },
        session.getSessionId(),
        NumberLong(13));
    coll.drop();
}

{
    const testName = jsTestName();
    const replTest = new ReplSetTest({
        name: testName,
        nodes: [{}, {rsConfig: {priority: 0}}],
        nodeOptions: {
            setParameter: {
                // Required for serverStatus() to have opWriteConcernCounters.
                reportOpWriteConcernCountersInServerStatus: true
            }
        }
    });

    replTest.startSet();
    replTest.initiateWithHighElectionTimeout();

    const retryCount = 3;
    for (const bulkWrite of [false, true]) {
        for (const timeseries of [false, true]) {
            runTest(false /* isMongos */, replTest, bulkWrite, retryCount, timeseries);
        }
    }

    replTest.stopSet();
}

{
    const st = new ShardingTest({mongos: 1, shards: 3, rs: {nodes: 1}, config: 1});

    const retryCount = 3;
    for (const bulkWrite of [false, true]) {
        for (const timeseries of [false, true]) {
            runTest(true /* isMongos */, st, bulkWrite, retryCount, timeseries);
        }
    }

    st.stop();
}
