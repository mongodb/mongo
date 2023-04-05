/**
 * Ensures that index builds are killed on primaries when the available disk space drops below a
 * limit.
 *
 * @tags: [
 *   featureFlagIndexBuildGracefulErrorHandling,
 *   requires_replication,
 * ]
 */
(function() {
"use strict";

load('jstests/noPassthrough/libs/index_build.js');
load("jstests/libs/fail_point_util.js");

const rst = new ReplSetTest({
    nodes: [
        {},
        {
            // Disallow elections on secondary.
            rsConfig: {
                priority: 0,
            },
        },
    ]
});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const primaryDB = primary.getDB('test');
const primaryColl = primaryDB.getCollection('test');

assert.commandWorked(primaryColl.insert({a: 1}));

let hangAfterInitFailPoint = configureFailPoint(primaryDB, 'hangAfterInitializingIndexBuild');

const tookActionCountBefore = primaryDB.serverStatus().metrics.diskSpaceMonitor.tookAction;

jsTestLog("Waiting for index build to start");
const createIdx = IndexBuildTest.startIndexBuild(
    primary, primaryColl.getFullName(), {a: 1}, null, [ErrorCodes.Interrupted]);
IndexBuildTest.waitForIndexBuildToStart(primaryDB, primaryColl.getName(), 'a_1');

// Ensure the index build is in an abortable state before the DiskSpaceMonitor runs.
hangAfterInitFailPoint.wait();

// Default indexBuildMinAvailableDiskSpaceMB is 500 MB.
// Simulate a remaining disk space of 450MB.
const simulateDiskSpaceFp =
    configureFailPoint(primaryDB, 'simulateAvailableDiskSpace', {bytes: 450 * 1024 * 1024});

jsTestLog("Waiting for the disk space monitor to take action");
assert.soon(() => {
    return primaryDB.serverStatus().metrics.diskSpaceMonitor.tookAction > tookActionCountBefore;
});
hangAfterInitFailPoint.off();

jsTestLog("Waiting for the index build to be killed");
// "Index build: joined after abort".
checkLog.containsJson(primary, 20655);

jsTestLog("Waiting for threads to join");
createIdx();
simulateDiskSpaceFp.off();

assert.eq(1, primaryDB.serverStatus().indexBuilds.killedDueToInsufficientDiskSpace);

rst.awaitReplication();
IndexBuildTest.assertIndexes(primaryColl, 1, ['_id_']);

const secondaryColl = rst.getSecondary().getCollection(primaryColl.getFullName());
IndexBuildTest.assertIndexes(secondaryColl, 1, ['_id_']);

rst.stopSet();
})();
