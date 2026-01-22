/**
 * TODO (SERVER-116499): Remove this file once 9.0 becomes last LTS.
 *
 * Tests that when the prepare phase fails on one of the shards during timeseries
 * upgrade/downgrade, the coordinator aborts correctly and no shard converts the collection.
 *
 * @tags: [
 *   requires_timeseries,
 *   requires_sharding,
 *   featureFlagCreateViewlessTimeseriesCollections,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {
    getTimeseriesBucketsColl,
    getTimeseriesCollForDDLOps,
} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";

if (lastLTSFCV != "8.0") {
    jsTest.log.info("Skipping test because last LTS FCV is no longer 8.0");
    quit();
}

const st = new ShardingTest({shards: 2, rs: {nodes: 1}});

const dbName = jsTestName();
const collName = "shardedTS";
const nss = dbName + "." + collName;

const db = st.s.getDB(dbName);
const adminDB = st.s.getDB("admin");
const coll = db.getCollection(collName);

assert.commandWorked(st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));

// Create sharded timeseries collection (in viewless format)
assert.commandWorked(
    st.s.adminCommand({
        shardCollection: nss,
        key: {m: 1},
        timeseries: {timeField: "t", metaField: "m"},
    }),
);

// Move range to shard1
assert.commandWorked(
    st.s.adminCommand({
        moveRange: getTimeseriesCollForDDLOps(db, coll).getFullName(),
        min: {meta: 200},
        max: {meta: MaxKey},
        toShard: st.shard1.shardName,
    }),
);

// Insert data to both shards
assert.commandWorked(
    coll.insertMany([
        {t: ISODate(), m: 100, foo: 1},
        {t: ISODate(), m: 150, foo: 2},
        {t: ISODate(), m: 300, foo: 3},
        {t: ISODate(), m: 400, foo: 4},
    ]),
);

// Verify collection is in viewless format (no system.buckets collection exists)
jsTest.log.info("Verifying collection starts in viewless format...");
for (const rs of [st.rs0, st.rs1]) {
    const shardColl = rs.getPrimary().getDB(dbName).getCollection(collName);
    assert(!getTimeseriesBucketsColl(shardColl).exists());
}

// Set FCV to lastLTS to disable the feature flag (required for downgrade command)
assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}));

// Enable failpoint on shard1 to fail the participant preparecommand
const failCommandFP = configureFailPoint(st.rs1.getPrimary(), "failCommand", {
    failCommands: ["_shardsvrTimeseriesUpgradeDowngradePrepare"],
    errorCode: ErrorCodes.InternalError,
    failInternalCommands: true,
    namespace: nss,
});

// Run downgrade - should fail with InternalError (injected by failpoint)
assert.commandFailedWithCode(
    db.runCommand({
        upgradeDowngradeViewlessTimeseries: collName,
        mode: "downgradeToLegacy",
    }),
    ErrorCodes.InternalError,
    "Downgrade should have failed due to failpoint",
);

jsTest.log.info("Verifying both shards are still in viewless format (no system.buckets)...");
for (const rs of [st.rs0, st.rs1]) {
    const shardColl = rs.getPrimary().getDB(dbName).getCollection(collName);
    assert(!getTimeseriesBucketsColl(shardColl).exists());
}

jsTest.log.info("Verifying critical section released...");
assert.eq(coll.countDocuments({}), 4);
assert.commandWorked(coll.insertOne({t: ISODate(), m: 500, foo: 5}));
assert.eq(coll.countDocuments({}), 5);

jsTest.log.info("Verifying migrations resumed...");
const configDB = st.s.getDB("config");
const collDoc = configDB.collections.findOne({_id: nss});
assert(collDoc);
assert.eq(undefined, collDoc.allowMigrations);

failCommandFP.off();

st.stop();
