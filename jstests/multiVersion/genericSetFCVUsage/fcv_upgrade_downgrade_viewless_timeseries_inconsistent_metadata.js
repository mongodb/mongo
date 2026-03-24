/**
 * Tests that setFCV succeeds even when some timeseries collections have metadata inconsistencies.
 *
 * TODO (SERVER-116499): Remove this file once 9.0 becomes last LTS.
 *
 * @tags: [
 *   requires_timeseries,
 *   featureFlagCreateViewlessTimeseriesCollections,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

if (lastLTSFCV != "8.0") {
    jsTest.log.info("Skipping test because last LTS FCV is no longer 8.0");
    quit();
}

const st = new ShardingTest({shards: 1, rs: {nodes: 1}});

const dbName = jsTestName();
const db = st.s.getDB(dbName);
const adminDB = st.s.getDB("admin");

assert(FeatureFlagUtil.isPresentAndEnabled(db, "CreateViewlessTimeseriesCollections"));
assert.commandWorked(st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));

// Checks if collection is in viewless format.
function isViewlessTimeseriesFormat(collName) {
    const collInfo = db.getCollectionInfos({name: collName})[0];
    const bucketsExists =
        adminDB
            .getSiblingDB(dbName)
            .getCollection("system.buckets." + collName)
            .exists() !== null;
    return collInfo && collInfo.options && collInfo.options.timeseries && !bucketsExists;
}

const validTsCollName = "validTs";
const regularCollName = "regularColl";
const inconsistentCollName = "system.buckets." + regularCollName;
const primaryShard = st.rs0.getPrimary();
const shardDB = primaryShard.getDB(dbName);

jsTest.log.info("Downgrading FCV to lastLTS to create legacy timeseries collections");
assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}));

jsTest.log.info("Creating valid timeseries collection in legacy format");
assert.commandWorked(
    db.createCollection(validTsCollName, {
        timeseries: {timeField: "t", metaField: "m"},
    }),
);
assert.commandWorked(db[validTsCollName].insertOne({t: ISODate(), m: 1, value: "valid"}));

// Create a regular collection.
assert.commandWorked(db.createCollection(regularCollName));

// Create the orphan system.buckets collection.
const createBucketsOp = {
    op: "c",
    ns: dbName + ".$cmd",
    o: {
        create: inconsistentCollName,
        clusteredIndex: true,
        timeseries: {
            timeField: "t",
            granularity: "seconds",
            bucketMaxSpanSeconds: 3600,
        },
    },
};
const fp = configureFailPoint(primaryShard, "skipCheckConflictingTimeseriesNamespace", {
    namespace: dbName + "." + inconsistentCollName,
});
assert.commandWorked(shardDB.runCommand({applyOps: [createBucketsOp]}));
fp.off();

jsTest.log.info("Attempting FCV upgrade, this should succeed despite the metadata inconsistency");
assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));

jsTest.log.info("Verifying valid collection was converted to viewless format");
assert.eq(true, isViewlessTimeseriesFormat(validTsCollName));

jsTest.log.info("Verifying regular collection remains non-timeseries and orphan buckets are kept");
const regularCollInfo = db.getCollectionInfos({name: regularCollName})[0];
assert(regularCollInfo);
assert(!regularCollInfo.options?.timeseries);
assert.neq(null, adminDB.getSiblingDB(dbName).getCollection(inconsistentCollName).exists());

jsTest.log.info("Verifying bypass parameter allowDirectSystemBucketsAccess works");
assert.commandFailedWithCode(
    db.runCommand({count: inconsistentCollName}),
    ErrorCodes.CommandNotSupportedOnLegacyTimeseriesBucketsNamespace,
);

assert.commandWorked(primaryShard.adminCommand({setParameter: 1, allowDirectSystemBucketsAccess: true}));
assert.commandWorked(db.runCommand({count: inconsistentCollName}));

// cleanup
assert.commandWorked(primaryShard.adminCommand({setParameter: 1, allowDirectSystemBucketsAccess: false}));
assert.commandWorked(db.dropDatabase());

st.stop();
