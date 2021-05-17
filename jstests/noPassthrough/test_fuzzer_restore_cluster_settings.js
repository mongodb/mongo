/**
 * Tests that the run_fuzzer_restore_cluster_settings.js hook successfully restores sane
 * cluster-wide default read and write concerns.
 *
 * @tags: [requires_replication]
 */

// The run_fuzzer_restore_cluster_settings.js hook depends on the global `db` object being set.
var db;

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");  // For configureFailPoint.

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
db = primary.getDB("test");

function testFuzzerRestoreClusterSettings() {
    load("jstests/hooks/run_fuzzer_restore_cluster_settings.js");

    const res = assert.commandWorked(primary.adminCommand({getDefaultRWConcern: 1}));
    assert.eq(undefined, res.defaultReadConcern, res);
    assert.eq(undefined, res.defaultWriteConcern, res);
}

// Test restoring settings when the cluster is in latestFCV.
assert.commandWorked(primary.adminCommand({
    setDefaultRWConcern: 1,
    defaultReadConcern: {level: "majority"},
    defaultWriteConcern: {w: 2},
}));
testFuzzerRestoreClusterSettings();

// Test restoring settings when the cluster is in the midst of downgrading to lastStableFCV.
configureFailPoint(primary, "failDowngrading", undefined, {times: 1});
assert.commandFailed(primary.adminCommand({setFeatureCompatibilityVersion: lastStableFCV}));
checkFCV(primary.getDB("admin"), lastStableFCV, lastStableFCV);
testFuzzerRestoreClusterSettings();

// Test restoring settings when the cluster is in lastStableFCV.
assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: lastStableFCV}));
testFuzzerRestoreClusterSettings();

rst.stopSet();
})();
