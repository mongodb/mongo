/**
 * Tests that setFCV fails when timeseries conversion exceeds the maximum retry limit.
 *
 * Uses a failpoint to simulate a persistent failure where timeseries collections
 * can never be fully converted, verifying that setFCV doesn't loop forever and
 * instead fails with a clear error after exhausting retries.
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

assert(FeatureFlagUtil.isPresentAndEnabled(db, "CreateViewlessTimeseriesCollections"));
assert.commandWorked(st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));

jsTest.log.info("Creating a timeseries collection in viewless format");
const testColl = db["testTs"];
assert.commandWorked(
    db.createCollection("testTs", {
        timeseries: {timeField: "t", metaField: "m"},
    }),
);
assert.commandWorked(testColl.insertOne({t: ISODate(), m: 1, value: "test"}));

jsTest.log.info("Enabling failpoint to make timeseries conversion always find collections");
const failpoint = configureFailPoint(st.configRS.getPrimary(), "alwaysReportTimeseriesCollectionsNeedConversion");

jsTest.log.info("Attempting FCV downgrade, should fail after exhausting retries");
assert.commandFailedWithCode(st.s.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}), 11481600);

jsTest.log.info("Disabling failpoint");
failpoint.off();

jsTest.log.info("Verifying setFCV now succeeds without the failpoint");
assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}));

// Upgrade back to verify everything is working
assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));

st.stop();
