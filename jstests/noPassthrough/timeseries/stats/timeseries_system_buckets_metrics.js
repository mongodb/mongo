/**
 * Validates metrics.numCommandsTargetingSystemBuckets when issuing commands that
 * target timeseries collections vs directly targeting system.buckets collections.
 *
 * // TODO SERVER-121176: remove this test once 9.0 becomes last LTS.
 * @tags: [
 *   requires_timeseries,
 * ]
 */

import {
    areViewlessTimeseriesEnabled,
    getTimeseriesBucketsColl,
} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const timeField = "my_time";
const metaField = "my_meta";

const timeseriesRawDoc = {
    "_id": ObjectId("67cc72e03dc19da48a654135"),
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
    // When featureFlagBlockDirectSystemBucketsAccess is enabled, commands are rejected after the metric
    // is incremented, so the count remains the same regardless of whether the commands succeed.
    coll = testDB.getCollection(getTimeseriesBucketsColl("coll"));
    const kBlockedErrorCode = ErrorCodes.CommandNotSupportedOnLegacyTimeseriesBucketsNamespace;
    assert.commandWorkedOrFailedWithCode(
        testDB.createCollection(coll.getName(), {timeseries: {timeField, metaField}}),
        kBlockedErrorCode,
    );
    assert.commandWorkedOrFailedWithCode(
        testDB.runCommand({insert: coll.getName(), documents: [timeseriesRawDoc]}),
        kBlockedErrorCode,
    );
    assert.commandWorkedOrFailedWithCode(testDB.runCommand({drop: coll.getName()}), kBlockedErrorCode);

    const EXPECTED_DIRECT_BUCKET_COMMANDS = 3; // createCollection + insert + drop
    assertSystemBucketsMetrics(
        primaryConn,
        otherConns,
        EXPECTED_DIRECT_BUCKET_COMMANDS,
        `[after direct system.buckets ops] (${name})`,
    );
}

// TODO SERVER-119235: Remove once chunk-related sharding commands support rawData.
function testShardingCommandsNotCounted(st) {
    jsTest.log.info("Testing that sharding commands allowed to target system.buckets do not increment the counter");

    if (areViewlessTimeseriesEnabled(st.s.getDB("admin"))) {
        return;
    }

    const dbName = `${jsTestName()}_sharding_cmds`;
    const testDB = st.s.getDB(dbName);
    const coll = testDB.getCollection("ts");
    const bucketNss = getTimeseriesBucketsColl(coll).getFullName();

    assert.commandWorked(st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
    assert.commandWorked(
        st.s.adminCommand({
            shardCollection: coll.getFullName(),
            key: {[metaField]: 1},
            timeseries: {timeField, metaField},
        }),
    );

    const allConns = [st.s, st.rs0.getPrimary(), st.rs1.getPrimary()];
    const baselines = allConns.map((conn) => getNumSystemBucketsCommands(conn));

    assert.commandWorked(st.s.adminCommand({split: bucketNss, middle: {meta: 0}}));
    assert.commandWorked(st.s.adminCommand({clearJumboFlag: bucketNss, find: {meta: -1}}));
    assert.commandWorked(
        st.s.adminCommand({moveChunk: bucketNss, find: {meta: 0}, to: st.shard1.shardName, _waitForDelete: true}),
    );
    assert.commandWorked(
        st.s.adminCommand({moveRange: bucketNss, min: {meta: 0}, max: {meta: MaxKey}, toShard: st.shard0.shardName}),
    );
    assert.commandWorked(st.s.adminCommand({mergeChunks: bucketNss, bounds: [{meta: MinKey}, {meta: MaxKey}]}));
    assert.commandWorked(st.s.adminCommand({mergeAllChunksOnShard: bucketNss, shard: st.shard0.shardName}));
    assert.commandWorked(st.rs0.getPrimary().adminCommand({cleanupOrphaned: bucketNss}));
    assert.commandWorked(st.s.adminCommand({balancerCollectionStatus: bucketNss}));
    assert.commandWorked(st.s.adminCommand({configureCollectionBalancing: bucketNss}));

    for (let i = 0; i < allConns.length; i++) {
        assert.eq(
            baselines[i],
            getNumSystemBucketsCommands(allConns[i]),
            `Sharding command incremented system.buckets counter on ${allConns[i].host}`,
        );
    }

    assert.commandWorked(testDB.dropDatabase());
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
        testShardingCommandsNotCounted(st);
    } finally {
        st.stop();
    }
}
