/**
 * Ensures that index builds are killed on primaries when the available disk space drops below a
 * limit,only if the primary has not yet voted for commit.
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

function killBeforeVoteCommitSucceeds(rst) {
    const primary = rst.getPrimary();
    const primaryDB = primary.getDB('test');
    const primaryColl = primaryDB.getCollection('test');

    primaryColl.drop();
    assert.commandWorked(primaryColl.insert({a: 1}));

    const hangAfterInitFailPoint = configureFailPoint(primaryDB, 'hangAfterInitializingIndexBuild');

    let serverStatus = primaryDB.serverStatus();
    const tookActionCountBefore = serverStatus.metrics.diskSpaceMonitor.tookAction;
    const killedDueToInsufficientDiskSpaceBefore =
        serverStatus.indexBuilds.killedDueToInsufficientDiskSpace;

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

    jsTestLog("Waiting for the index build to be killed");
    // "Index build: joined after abort".
    checkLog.containsJson(primary, 20655);

    jsTestLog("Waiting for threads to join");
    createIdx();
    simulateDiskSpaceFp.off();
    hangAfterInitFailPoint.off();

    // "Index build: aborted due to insufficient disk space"
    checkLog.containsJson(primary, 7333601);

    assert.eq(killedDueToInsufficientDiskSpaceBefore + 1,
              primaryDB.serverStatus().indexBuilds.killedDueToInsufficientDiskSpace);

    rst.awaitReplication();
    IndexBuildTest.assertIndexes(primaryColl, 1, ['_id_']);

    const secondaryColl = rst.getSecondary().getCollection(primaryColl.getFullName());
    IndexBuildTest.assertIndexes(secondaryColl, 1, ['_id_']);
}

function killAfterVoteCommitFails(rst) {
    const primary = rst.getPrimary();
    const primaryDB = primary.getDB('test');
    const primaryColl = primaryDB.getCollection('test');

    primaryColl.drop();
    assert.commandWorked(primaryColl.insert({a: 1}));

    const hangAfterVoteCommit =
        configureFailPoint(primaryDB, 'hangIndexBuildAfterSignalPrimaryForCommitReadiness');

    let serverStatus = primaryDB.serverStatus();
    const tookActionCountBefore = serverStatus.metrics.diskSpaceMonitor.tookAction;
    const killedDueToInsufficientDiskSpaceBefore =
        serverStatus.indexBuilds.killedDueToInsufficientDiskSpace;

    jsTestLog("Waiting for index build to start");
    const createIdx = IndexBuildTest.startIndexBuild(
        primary, primaryColl.getFullName(), {a: 1}, null, [ErrorCodes.Interrupted]);
    IndexBuildTest.waitForIndexBuildToStart(primaryDB, primaryColl.getName(), 'a_1');

    // Ensure the index build has voted commit before the DiskSpaceMonitor runs.
    hangAfterVoteCommit.wait();

    // Default indexBuildMinAvailableDiskSpaceMB is 500 MB.
    // Simulate a remaining disk space of 450MB.
    const simulateDiskSpaceFp =
        configureFailPoint(primaryDB, 'simulateAvailableDiskSpace', {bytes: 450 * 1024 * 1024});

    jsTestLog("Waiting for the disk space monitor to take action");
    assert.soon(() => {
        return primaryDB.serverStatus().metrics.diskSpaceMonitor.tookAction > tookActionCountBefore;
    });

    jsTestLog("Waiting for the index build kill attempt to fail");
    // "Index build: cannot force abort".
    checkLog.containsJson(primary, 7617000);

    hangAfterVoteCommit.off();
    simulateDiskSpaceFp.off();

    jsTestLog("Waiting for threads to join");
    createIdx();

    assert.eq(killedDueToInsufficientDiskSpaceBefore,
              primaryDB.serverStatus().indexBuilds.killedDueToInsufficientDiskSpace);

    rst.awaitReplication();
    IndexBuildTest.assertIndexes(primaryColl, 2, ['_id_', 'a_1']);

    const secondaryColl = rst.getSecondary().getCollection(primaryColl.getFullName());
    IndexBuildTest.assertIndexes(secondaryColl, 2, ['_id_', 'a_1']);
}

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

killBeforeVoteCommitSucceeds(rst);
killAfterVoteCommitFails(rst);

rst.stopSet();
})();
