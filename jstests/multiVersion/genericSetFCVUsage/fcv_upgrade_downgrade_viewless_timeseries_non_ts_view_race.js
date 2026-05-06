/**
 * TODO (SERVER-116499): Remove this file once 9.0 becomes last LTS.
 *
 * Regression test for SERVER-119484.
 *
 * During FCV downgrade (9.0 → 8.0), the config server enumerates viewless timeseries collections
 * and converts them to legacy (view-backed) format. There is a race window between enumeration
 * and the per-collection shard-side coordinator's precondition check. If a concurrent DDL
 * operation drops the viewless collection and creates a plain (non-timeseries) view with the same
 * name in that window, the coordinator calls acquireCollectionWithBucketsLookup() on the
 * namespace, which throws CommandNotSupportedOnView (code 166) because the view is not a
 * timeseries view. Before the fix, this error was not caught by the outer retry loop in
 * upgradeDowngradeViewlessTimeseriesInShardedCluster(), so it propagated all the way back to the
 * setFCV caller and aborted the downgrade.
 *
 * The test uses the existing hangAfterEnumeratingTimeseriesCollectionsForFCV failpoint to
 * deterministically open the race window.
 *
 * @tags: [
 *   requires_timeseries,
 *   uses_parallel_shell,
 *   requires_replication,
 *   requires_sharding,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

// This test is only meaningful while 8.0 is the last-LTS FCV target, because viewless timeseries
// collections are a 9.0 feature and the conversion only runs on the 9.0 → 8.0 downgrade path.
if (lastLTSFCV !== "8.0") {
    jsTest.log.info("Skipping test: last-LTS FCV is no longer 8.0, test is obsolete.");
    quit();
}

// ---- Cluster setup ----

const st = new ShardingTest({shards: 1, rs: {nodes: 1}});

const dbName = jsTestName();
const testDB = st.s.getDB(dbName);
const adminDB = st.s.getDB("admin");

// The test only applies when the viewless timeseries feature flag is enabled (i.e. FCV >= 9.0).
if (!FeatureFlagUtil.isPresentAndEnabled(testDB, "CreateViewlessTimeseriesCollections")) {
    jsTest.log.info("Skipping test: CreateViewlessTimeseriesCollections feature flag is not enabled.");
    st.stop();
    quit();
}

// Place the test database on shard0 so the config server knows where to send commands.
assert.commandWorked(st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));

// ---- Test data ----

// Create a viewless timeseries collection. In FCV 9.0 this is a plain collection; there is no
// companion view or system.buckets.* collection.
const tsCollName = "ts";
const otherCollName = "other";

// Create a plain collection that the view will reference (not required to exist, but cleaner).
assert.commandWorked(testDB.createCollection(otherCollName));

assert.commandWorked(testDB.createCollection(tsCollName, {timeseries: {timeField: "t"}}));
assert.commandWorked(testDB[tsCollName].insertOne({t: ISODate(), x: 1}));

// Confirm viewless format: system.buckets.ts must not exist.
assert.eq(
    null,
    testDB["system.buckets." + tsCollName].exists(),
    "Expected viewless format before downgrade: system.buckets." + tsCollName + " should not exist.",
);

// ---- Race condition setup ----

// Enable the failpoint on the config server primary. It fires inside
// upgradeDowngradeViewlessTimeseriesInShardedCluster(), immediately after
// getAllClusterTimeseriesNamespaces() returns the list of viewless collections to convert, but
// before any ShardsvrUpgradeDowngradeViewlessTimeseries command is sent to any shard.
const configPrimary = st.configRS.getPrimary();
const hangFp = configureFailPoint(configPrimary, "hangAfterEnumeratingTimeseriesCollectionsForFCV");

// Kick off FCV downgrade in a parallel shell. The parallel shell connects through the mongos.
const awaitSetFCV = startParallelShell(
    funWithArgs(function (targetFCV) {
        // Before the fix this call throws:
        //   CommandNotSupportedOnView (code 166): "Namespace <db>.ts is a view, not a collection"
        // After the fix it succeeds: the race-created plain view is skipped gracefully.
        assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: targetFCV, confirm: true}));
    }, lastLTSFCV),
    st.s.port,
);

// Wait until setFCV has frozen: the list of viewless collections is in memory but no shard
// command has been dispatched yet.
jsTest.log.info("Waiting for failpoint: setFCV has enumerated collections, about to convert.");
hangFp.wait();

// While setFCV is paused, open the race window:
//   1. Drop the viewless timeseries collection.
//   2. Create a plain (non-timeseries) view with the same name, pointing to another collection.
//
// After this, the namespace `ts` exists in the local catalog as a view whose timeseries()
// getter returns false. The coordinator's acquireCollectionWithBucketsLookup() will see a view
// that is not a timeseries view and throw CommandNotSupportedOnView.
jsTest.log.info("Dropping viewless timeseries collection and creating a plain view with the same name.");
assert.commandWorked(testDB.runCommand({drop: tsCollName}));
assert.commandWorked(testDB.createView(tsCollName, otherCollName, []));

// Verify the namespace is now a plain view.
const viewInfo = testDB.getCollectionInfos({name: tsCollName});
assert.eq(1, viewInfo.length, "Expected one namespace info entry for '" + tsCollName + "'.");
assert.eq("view", viewInfo[0].type, "Expected '" + tsCollName + "' to be a view.");
assert(
    !viewInfo[0].options.timeseries,
    "Expected the view to have no timeseries options (it is a plain view, not a TS view).",
);

jsTest.log.info("Plain view created; releasing failpoint so setFCV can attempt the conversion.");
hangFp.off();

// Wait for setFCV to finish. With the fix applied, it should complete successfully: the outer
// retry loop in upgradeDowngradeViewlessTimeseriesInShardedCluster() catches
// CommandNotSupportedOnView and skips the namespace, treating it like any other namespace that
// is no longer a timeseries collection.
awaitSetFCV();

jsTest.log.info("setFCV(8.0) completed successfully.");

// The plain view should be untouched — setFCV should not have modified or dropped it.
const viewInfoAfter = testDB.getCollectionInfos({name: tsCollName});
assert.eq(1, viewInfoAfter.length, "Expected the plain view to still exist after setFCV.");
assert.eq("view", viewInfoAfter[0].type, "Expected '" + tsCollName + "' to remain a plain view.");

// Upgrade back to latestFCV so the cluster can shut down cleanly.
assert.commandWorked(adminDB.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));

st.stop();
