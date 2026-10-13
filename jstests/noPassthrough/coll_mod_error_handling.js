/**
 * Tests collMod error handling for non-retriable errors. Validates that cleanup will be triggered
 * for non-tracked, timeseries modifications and that the coordinator must always make progress for
 * tracked, timeseries modifications.
 *
 * @tags: [
 *   requires_sharding,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";

function checkCleanupComplete(configDB, dbPrimary, ns) {
    const collEntry = configDB.collections.findOne({_id: ns});
    assert(collEntry, "expected sharded timeseries collection in config.collections");
    assert.eq(
        undefined,
        collEntry.allowMigrations,
        "migrations should be unblocked after collmod cleans up",
        {collEntry},
    );
    const criticalSection = dbPrimary
        .getCollection("config.collection_critical_sections")
        .findOne({_id: ns});
    assert.eq(
        null,
        criticalSection,
        "db primary shard should release the critical section before the coordinator completes",
    );
}

const dbName = jsTestName();
const collName = "timeseriesColl";
const timeField = "tm";
const metaField = "mt";
const timeseriesBucketNs = "system.buckets." + collName;

const st = new ShardingTest({shards: 2, rs: {nodes: 2}});
const db = st.s.getDB(dbName);

assert.commandWorked(
    st.s.adminCommand({
        enableSharding: dbName,
        primaryShard: st.shard0.shardName,
    }),
);
assert.commandWorked(
    db.createCollection(collName, {
        timeseries: {
            timeField: timeField,
            metaField: metaField,
            granularity: "seconds",
        },
    }),
);
assert.commandWorked(
    db.adminCommand({
        shardCollection: `${dbName}.${collName}`,
        key: {[metaField]: 1},
    }),
);
assert.commandWorked(
    db.getCollection(timeseriesBucketNs).createIndex({a: 1}),
);

const targetingNs = `${dbName}.${timeseriesBucketNs}`;
const configDB = st.s.getDB("config");
const dbPrimary = st.rs0.getPrimary();

(function testCoordinatorCleansUpDuringBlockShards() {
    const failPoint = configureFailPoint(
        dbPrimary,
        "throwErrorDuringBlockShardsPhase",
        {},
        {times: 1},
    );
    assert.commandFailedWithCode(
        st.s.getDB(dbName).runCommand({collMod: collName, timeseries: {granularity: "minutes"}}),
        ErrorCodes.BadValue,
        "collMod should fail with BadValue when the failpoint is enabled",
    );
    failPoint.off();
    checkCleanupComplete(configDB, dbPrimary, targetingNs);
})();

(function testCoordinatorRetriesDuringConfigUpdate() {
    const failPoint = configureFailPoint(
        dbPrimary,
        "throwErrorDuringConfigUpdatePhase",
        {},
        {times: 1},
    );
    assert.commandWorked(
        st.s.getDB(dbName).runCommand({collMod: collName, timeseries: {granularity: "minutes"}}),
    );
    failPoint.off();

    checkCleanupComplete(configDB, dbPrimary, targetingNs);
})();

(function testCoordinatorRetriesDuringUpdateShards() {
    const failPoint = configureFailPoint(
        dbPrimary,
        "throwErrorDuringUpdateShardsPhase",
        {},
        {times: 1},
    );
    assert.commandWorked(
        st.s.getDB(dbName).runCommand({collMod: collName, timeseries: {granularity: "hours"}}),
    );
    failPoint.off();

    checkCleanupComplete(configDB, dbPrimary, targetingNs);
})();

(function testCoordinatorCleansUpForNonTrackedModifications() {
    const failPoint = configureFailPoint(dbPrimary, "throwErrorDuringUpdateShardsPhase");
    assert.commandFailedWithCode(
        st.s
            .getDB(dbName)
            .runCommand({collMod: collName, index: {keyPattern: {a: 1}, hidden: true}}),
        ErrorCodes.BadValue,
        "collMod should fail with BadValue when the failpoint is enabled",
    );
    failPoint.off();

    checkCleanupComplete(configDB, dbPrimary, targetingNs);
})();

st.stop();
