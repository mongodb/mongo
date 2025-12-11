/**
 * Validates metrics.numCommandsTargetingSystemBuckets when issuing commands that
 * target timeseries collections vs directly targeting system.buckets collections.
 *
 * @tags: [
 *   requires_timeseries,
 * ]
 */

import {getTimeseriesBucketsColl} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const timeField = "my_time";
const metaField = "my_meta";

const timeseriesRawDoc = {
    "control": {
        "version": 2,
        "min": {
            "_id": ObjectId("68f24fb53dc19da48a654135"),
            "my_time": ISODate("2025-03-08T16:40:00Z"),
        },
        "max": {
            "_id": ObjectId("68f24fb53dc19da48a654135"),
            "my_time": ISODate("2025-03-08T16:40:39.239Z"),
        },
        "count": 1,
    },
    "meta": {
        "sensorId": 3,
    },
    "data": {
        "_id": BinData(7, "BwBo8k+1PcGdpIplQTUA"),
        "my_time": BinData(7, "CQBHVKF2lQEAAAA="),
    },
};

function getNumSystemBucketsCommands(conn) {
    const ss = assert.commandWorked(conn.getDB("admin").runCommand({serverStatus: 1}));
    assert.hasFields(ss, ["metrics"], "serverStatus missing 'metrics'");
    assert.hasFields(
        ss.metrics,
        ["numCommandsTargetingSystemBuckets"],
        "serverStatus.metrics missing 'numCommandsTargetingSystemBuckets'",
    );
    return ss.metrics.numCommandsTargetingSystemBuckets;
}

function assertSystemBucketsMetrics(primaryConn, otherConns, expectedPrimaryCount, ctxMsg) {
    const actualPrimaryCount = getNumSystemBucketsCommands(primaryConn);
    assert.eq(
        expectedPrimaryCount,
        actualPrimaryCount,
        `Unexpected count on (${primaryConn.host}) ${ctxMsg}. ` +
            `Expected ${expectedPrimaryCount}, got ${actualPrimaryCount}`,
    );

    for (const conn of otherConns) {
        const count = getNumSystemBucketsCommands(conn);
        assert.eq(0, count, `Expected 0 on connection (${conn.host}) ${ctxMsg}, got ${count}`);
    }
}

function testDirectBucketTargeting({name, primaryConn, otherConns}) {
    jsTest.log.info(`Testing direct bucket targeting metrics on topology: ${name}`);

    const dbName = `${jsTestName()}_${name}`;
    const testDB = primaryConn.getDB(dbName);

    // 1) Regular timeseries usage should NOT count as direct system.buckets targeting.
    let coll = testDB.getCollection("ts");
    assert.commandWorked(testDB.createCollection(coll.getName(), {timeseries: {timeField, metaField}}));
    assert.commandWorked(coll.insertOne({[timeField]: ISODate("2030-08-08T04:11:10Z")}));
    assert(coll.drop());

    assertSystemBucketsMetrics(primaryConn, otherConns, 0, `[after regular timeseries ops] (${name})`);

    // 2) Directly targeting system.buckets should be counted.
    coll = testDB.getCollection(getTimeseriesBucketsColl("coll"));
    assert.commandWorked(testDB.createCollection(coll.getName(), {timeseries: {timeField, metaField}}));
    assert.commandWorked(coll.insertOne(timeseriesRawDoc));
    assert(coll.drop());

    const EXPECTED_DIRECT_BUCKET_COMMANDS = 3; // createCollection + insert + drop
    assertSystemBucketsMetrics(
        primaryConn,
        otherConns,
        EXPECTED_DIRECT_BUCKET_COMMANDS,
        `[after direct system.buckets ops] (${name})`,
    );
}

// Standalone mongod.
{
    const mongod = MongoRunner.runMongod();
    try {
        testDirectBucketTargeting({name: "standalone", primaryConn: mongod, otherConns: []});
    } finally {
        MongoRunner.stopMongod(mongod);
    }
}

// Replica set.
{
    const rst = new ReplSetTest({name: jsTestName() + "_rs", nodes: 1});
    rst.startSet();
    rst.initiate();
    try {
        testDirectBucketTargeting({name: "replset", primaryConn: rst.getPrimary(), otherConns: []});
    } finally {
        rst.stopSet();
    }
}

// Sharded cluster with an additional config shard.
{
    const st = new ShardingTest({shards: 2, rs: {nodes: 1}, mongos: 1, configShard: true});
    try {
        testDirectBucketTargeting({
            name: "sharded",
            primaryConn: st.s,
            otherConns: [st.rs0.getPrimary(), st.rs1.getPrimary()],
        });
    } finally {
        st.stop();
    }
}
