/**
 * Ensures that index builds are cancelled by secondaries when the available disk space drops below
 * a limit, only if the secondary has not yet voted for commit.
 *
 * @tags: [
 *   # This test uses a failpoint that is only available for hybrid index builds.
 *   primary_driven_index_builds_incompatible,
 *   requires_fcv_71,
 *   requires_replication,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";

function killBeforeVoteCommitSucceeds(rst) {
    jsTestLog("Index build in a secondary can be killed by the DiskSpaceMonitor before it has voted for commit.");

    const dbName = "test";
    const collName = "coll";
    const primary = rst.getPrimary();
    const primaryDB = primary.getDB(dbName);
    const primaryColl = primaryDB.getCollection(collName);

    primaryColl.drop();
    assert.commandWorked(primaryColl.insert({a: 1}));

    rst.awaitReplication();

    const secondary = rst.getSecondary();
    const secondaryDB = secondary.getDB(dbName);
    const secondaryColl = secondaryDB.getCollection(collName);

    const primaryKilledDueToDiskSpaceBefore = primaryDB.serverStatus().indexBuilds.killedDueToInsufficientDiskSpace;
    const secondaryKilledDueToDiskSpaceBefore = secondaryDB.serverStatus().indexBuilds.killedDueToInsufficientDiskSpace;

    // Pause the index build on the primary after it replicates the startIndexBuild oplog entry,
    // effectively pausing the index build on the secondary too as it will wait for the primary to
    // commit or abort.
    IndexBuildTest.pauseIndexBuilds(primary);

    const tookActionCountBefore = secondaryDB.serverStatus().metrics.diskSpaceMonitor.tookAction;

    jsTestLog("Waiting for index build to start on secondary");
    const hangAfterInitFailPoint = configureFailPoint(secondaryDB, "hangAfterInitializingIndexBuild");
    const createIdx = IndexBuildTest.startIndexBuild(primary, primaryColl.getFullName(), {a: 1}, null, [
        ErrorCodes.IndexBuildAborted,
    ]);
    IndexBuildTest.waitForIndexBuildToStart(secondaryDB, secondaryColl.getName(), "a_1");

    // Ensure the index build is in an abortable state before the DiskSpaceMonitor runs.
    hangAfterInitFailPoint.wait();

    // Default indexBuildMinAvailableDiskSpaceMB is 500 MB.
    // Simulate a remaining disk space of 450MB on the secondary node.
    const simulateDiskSpaceFp = configureFailPoint(secondaryDB, "simulateAvailableDiskSpace", {
        bytes: 450 * 1024 * 1024,
    });

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

    // Confirm that the error message returned by the createIndexes command describes the secondary
    // running out of disk space, rather than a generic "operation was interrupted" message.
    // We use the log message as a proxy for the error message that is returned by createIndexes.
    checkLog.contains(
        primary,
        new RegExp(
            "20655.*Index build: joined after abort.*IndexBuildAborted.*'voteAbortIndexBuild' received from.*: available disk space of.*bytes is less than required minimum of",
        ),
    );

    simulateDiskSpaceFp.off();

    // "Index build: aborted due to insufficient disk space"
    checkLog.containsJson(secondaryDB, 7333601);

    // Disable failpoint only after we know the build is aborted. We want the build to be aborted
    // before it has voted for commit, and this ensures that is the case.
    hangAfterInitFailPoint.off();

    assert.eq(primaryKilledDueToDiskSpaceBefore, primaryDB.serverStatus().indexBuilds.killedDueToInsufficientDiskSpace);
    assert.eq(
        secondaryKilledDueToDiskSpaceBefore + 1,
        secondaryDB.serverStatus().indexBuilds.killedDueToInsufficientDiskSpace,
    );

    rst.awaitReplication();
    IndexBuildTest.assertIndexes(primaryColl, 1, ["_id_"]);
    IndexBuildTest.assertIndexes(secondaryColl, 1, ["_id_"]);
}

function killAfterVoteCommitFails(rst) {
    jsTestLog("Index build in a secondary cannot killed by the DiskSpaceMonitor after it has voted for commit");

    const dbName = "test";
    const collName = "coll";
    const primary = rst.getPrimary();
    const primaryDB = primary.getDB(dbName);
    const primaryColl = primaryDB.getCollection(collName);

    primaryColl.drop();
    assert.commandWorked(primaryColl.insert({a: 1}));

    rst.awaitReplication();

    const secondary = rst.getSecondary();
    const secondaryDB = secondary.getDB(dbName);
    const secondaryColl = secondaryDB.getCollection(collName);

    const primaryKilledDueToDiskSpaceBefore = primaryDB.serverStatus().indexBuilds.killedDueToInsufficientDiskSpace;
    const secondaryKilledDueToDiskSpaceBefore = secondaryDB.serverStatus().indexBuilds.killedDueToInsufficientDiskSpace;

    // Pause the index build on the primary after it replicates the startIndexBuild oplog entry,
    // effectively pausing the index build on the secondary too as it will wait for the primary to
    // commit or abort.
    IndexBuildTest.pauseIndexBuilds(primary);

    const tookActionCountBefore = secondaryDB.serverStatus().metrics.diskSpaceMonitor.tookAction;

    jsTestLog("Waiting for index build to start on secondary");
    const hangAfterVoteCommit = configureFailPoint(secondaryDB, "hangIndexBuildAfterSignalPrimaryForCommitReadiness");
    const createIdx = IndexBuildTest.startIndexBuild(primary, primaryColl.getFullName(), {a: 1}, null);
    IndexBuildTest.waitForIndexBuildToStart(secondaryDB, secondaryColl.getName(), "a_1");

    // Ensure the index build is in an abortable state before the DiskSpaceMonitor runs.
    hangAfterVoteCommit.wait();

    // Default indexBuildMinAvailableDiskSpaceMB is 500 MB.
    // Simulate a remaining disk space of 450MB on the secondary node.
    const simulateDiskSpaceFp = configureFailPoint(secondaryDB, "simulateAvailableDiskSpace", {
        bytes: 450 * 1024 * 1024,
    });

    jsTestLog("Waiting for the disk space monitor to take action on secondary");
    assert.soon(() => {
        return secondaryDB.serverStatus().metrics.diskSpaceMonitor.tookAction > tookActionCountBefore;
    });
    IndexBuildTest.resumeIndexBuilds(primary);

    jsTestLog("Waiting for the index build kill attempt to fail");
    // "Index build: cannot force abort".
    checkLog.containsJson(secondary, 7617000);

    // Disable failpoint only after the abort attempt.
    hangAfterVoteCommit.off();

    jsTestLog("Waiting for threads to join");
    createIdx();
    simulateDiskSpaceFp.off();

    assert.eq(primaryKilledDueToDiskSpaceBefore, primaryDB.serverStatus().indexBuilds.killedDueToInsufficientDiskSpace);
    assert.eq(
        secondaryKilledDueToDiskSpaceBefore,
        secondaryDB.serverStatus().indexBuilds.killedDueToInsufficientDiskSpace,
    );

    rst.awaitReplication();
    IndexBuildTest.assertIndexes(primaryColl, 2, ["_id_", "a_1"]);
    IndexBuildTest.assertIndexes(secondaryColl, 2, ["_id_", "a_1"]);
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
    ],
});
rst.startSet();
rst.initiate();

killBeforeVoteCommitSucceeds(rst);
killAfterVoteCommitFails(rst);

rst.stopSet();
