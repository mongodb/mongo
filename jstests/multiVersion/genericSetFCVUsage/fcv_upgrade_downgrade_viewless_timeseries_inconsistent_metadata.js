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

// Checks if system.buckets.<collName> exists, indicating legacy (viewful) format.
function isLegacyTimeseriesFormat(collName) {
    return (
        adminDB
            .getSiblingDB(dbName)
            .getCollection("system.buckets." + collName)
            .exists() !== null
    );
}

// Checks if collection is in viewless format (main namespace exists with timeseries options).
function isViewlessTimeseriesFormat(collName) {
    const collInfo = db.getCollectionInfos({name: collName})[0];
    return collInfo && collInfo.options && collInfo.options.timeseries && !isLegacyTimeseriesFormat(collName);
}

jsTest.log.info("Creating two viewless timeseries collections at latest FCV");
const validCollName = "validTs";
const inconsistentCollName = "inconsistentTs";
const validColl = db[validCollName];
const inconsistentColl = db[inconsistentCollName];

assert.commandWorked(
    db.createCollection(validCollName, {
        timeseries: {timeField: "t", metaField: "m"},
    }),
);
assert.commandWorked(validColl.insertOne({t: ISODate(), m: 1, value: "valid"}));

assert.commandWorked(
    db.createCollection(inconsistentCollName, {
        timeseries: {timeField: "t", metaField: "m"},
    }),
);
assert.commandWorked(inconsistentColl.insertOne({t: ISODate(), m: 2, value: "inconsistent"}));

jsTest.log.info("Downgrading FCV to last LTS to get legacy format collections");
assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}));

jsTest.log.info("Verifying both collections are now in legacy format");
assert.eq(true, isLegacyTimeseriesFormat(validCollName));
assert.eq(true, isLegacyTimeseriesFormat(inconsistentCollName));

jsTest.log.info("Enabling failpoint on CONFIG SERVER to simulate UserDataInconsistent");
const configPrimary = st.configRS.getPrimary();
const failpoint = configureFailPoint(configPrimary, "FailViewlessTimeseriesUpgradeWithUserDataInconsistent", {
    namespace: inconsistentColl.getFullName(),
});

jsTest.log.info("Attempting FCV upgrade, this should succeed despite one collection having issues");
assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));

jsTest.log.info("Verifying valid collection was converted to viewless format");
assert.eq(true, isViewlessTimeseriesFormat(validCollName));

jsTest.log.info("Verifying inconsistent collection was skipped (still in legacy format)");
assert.eq(true, isLegacyTimeseriesFormat(inconsistentCollName));

// Disable failpoint
failpoint.off();

st.stop();
