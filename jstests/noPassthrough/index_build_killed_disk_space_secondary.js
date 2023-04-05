/**
 * Ensures that index builds are cancelled by secondaries when the available disk space drops below
 * a limit.
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

const dbName = 'test';
const collName = 'coll';
const primary = rst.getPrimary();
const primaryDB = primary.getDB(dbName);
const primaryColl = primaryDB.getCollection(collName);

assert.commandWorked(primaryColl.insert({a: 1}));

rst.awaitReplication();

const secondary = rst.getSecondary();
const secondaryDB = secondary.getDB(dbName);
const secondaryColl = secondaryDB.getCollection(collName);

// Pause the index build on the primary after it replicates the startIndexBuild oplog entry,
// effectively pausing the index build on the secondary too as it will wait for the primary to
// commit or abort.
IndexBuildTest.pauseIndexBuilds(primary);

const tookActionCountBefore = secondaryDB.serverStatus().metrics.diskSpaceMonitor.tookAction;

jsTestLog("Waiting for index build to start on secondary");
const hangAfterInitFailPoint = configureFailPoint(secondaryDB, 'hangAfterInitializingIndexBuild');
const createIdx = IndexBuildTest.startIndexBuild(
    primary, primaryColl.getFullName(), {a: 1}, null, [ErrorCodes.IndexBuildAborted]);
IndexBuildTest.waitForIndexBuildToStart(secondaryDB, secondaryColl.getName(), 'a_1');

// Ensure the index build is in an abortable state before the DiskSpaceMonitor runs.
hangAfterInitFailPoint.wait();
hangAfterInitFailPoint.off();

// Default indexBuildMinAvailableDiskSpaceMB is 500 MB.
// Simulate a remaining disk space of 450MB on the secondary node.
const simulateDiskSpaceFp =
    configureFailPoint(secondaryDB, 'simulateAvailableDiskSpace', {bytes: 450 * 1024 * 1024});

jsTestLog("Waiting for the disk space monitor to take action on secondary");
assert.soon(() => {
    return secondaryDB.serverStatus().metrics.diskSpaceMonitor.tookAction > tookActionCountBefore;
});
IndexBuildTest.resumeIndexBuilds(primary);

jsTestLog("Waiting for the index build to be killed");
// "Index build: joined after abort".
checkLog.containsJson(secondary, 20655);

jsTestLog("Waiting for threads to join");
createIdx();
simulateDiskSpaceFp.off();

assert.eq(0, primaryDB.serverStatus().indexBuilds.killedDueToInsufficientDiskSpace);
assert.eq(1, secondaryDB.serverStatus().indexBuilds.killedDueToInsufficientDiskSpace);

rst.awaitReplication();
IndexBuildTest.assertIndexes(primaryColl, 1, ['_id_']);
IndexBuildTest.assertIndexes(secondaryColl, 1, ['_id_']);

rst.stopSet();
})();
