/**
 * Ensures that index builds cannot be aborted after voting for commit.
 *
 * @tags: [
 *   requires_fcv_71,
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
const hangBeforeVoteCommit = configureFailPoint(primary, 'hangBeforeVoteCommitIndexBuild');

const tookActionCountBefore = secondaryDB.serverStatus().metrics.diskSpaceMonitor.tookAction;

jsTestLog("Waiting for index build to start on secondary");
const createIdx = IndexBuildTest.startIndexBuild(
    primary, primaryColl.getFullName(), {a: 1}, null, [ErrorCodes.IndexBuildAborted]);
IndexBuildTest.waitForIndexBuildToStart(secondaryDB, secondaryColl.getName(), 'a_1');

// Wait until secondary is voting for commit.
hangBeforeVoteCommit.wait();

// Default indexBuildMinAvailableDiskSpaceMB is 500 MB.
// Simulate a remaining disk space of 450MB on the secondary node.
const simulateDiskSpaceFp =
    configureFailPoint(secondaryDB, 'simulateAvailableDiskSpace', {bytes: 450 * 1024 * 1024});

jsTestLog("Waiting for the disk space monitor to take action on secondary");
assert.soon(() => {
    return secondaryDB.serverStatus().metrics.diskSpaceMonitor.tookAction > tookActionCountBefore;
});
IndexBuildTest.resumeIndexBuilds(primary);

jsTestLog("Waiting for the index build kill attempt to fail");
// "Index build: cannot force abort".
checkLog.containsJson(secondary, 7617000);
hangBeforeVoteCommit.off();

jsTestLog("Waiting for threads to join");
createIdx();
simulateDiskSpaceFp.off();

assert.eq(0, primaryDB.serverStatus().indexBuilds.killedDueToInsufficientDiskSpace);
assert.eq(0, secondaryDB.serverStatus().indexBuilds.killedDueToInsufficientDiskSpace);

IndexBuildTest.assertIndexesSoon(primaryColl, 2, ['_id_', 'a_1']);
IndexBuildTest.assertIndexesSoon(secondaryColl, 2, ['_id_', 'a_1']);

rst.stopSet();
})();
