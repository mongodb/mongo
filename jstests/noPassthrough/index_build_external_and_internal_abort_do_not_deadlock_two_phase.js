/**
 * Tests dropping a collection (causing an external index build abort) does not deadlock with an
 * internal self abort for two-phase index builds.
 *
 * @tags: [
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

primaryColl.drop();
assert.commandWorked(primaryColl.insert({a: 1}));

// Pause the index builds on the secondary, using the 'hangAfterStartingIndexBuild' failpoint.
const failpointHangAfterInit = configureFailPoint(primaryDB, "hangAfterInitializingIndexBuild");
const hangBeforeCleanup = configureFailPoint(primaryDB, 'hangIndexBuildBeforeAbortCleanUp');

// Block secondary to avoid commitQuorum being fullfilled.
IndexBuildTest.pauseIndexBuilds(rst.getSecondary());

jsTestLog("Waiting for index build to start");
const createIdx = IndexBuildTest.startIndexBuild(
    primary, primaryColl.getFullName(), {a: 1}, null, [ErrorCodes.OutOfDiskSpace]);

const buildUUID =
    IndexBuildTest
        .assertIndexesSoon(primaryColl, 2, ['_id_'], ['a_1'], {includeBuildUUIDs: true})['a_1']
        .buildUUID;

const failAfterVoteForCommitReadiness =
    configureFailPoint(primaryDB,
                       "failIndexBuildWithErrorInSecondDrain",
                       {buildUUID: buildUUID, error: ErrorCodes.OutOfDiskSpace});

// Continue index build after preparing the artificial failure.
failpointHangAfterInit.off();

// Wait for the index build to be in clean up path.
hangBeforeCleanup.wait();

const hangAfterCollDropHasLocks =
    configureFailPoint(primaryDB, 'hangAbortIndexBuildByBuildUUIDAfterLocks');

const collDrop = startParallelShell(funWithArgs(function(dbName, collName) {
                                        jsTestLog("Dropping collection");
                                        db.getSiblingDB(dbName).getCollection(collName).drop();
                                    }, primaryDB.getName(), primaryColl.getName()), primary.port);

hangAfterCollDropHasLocks.wait();
hangBeforeCleanup.off();
hangAfterCollDropHasLocks.off();

// The index build should not be externally abortable once the index builder thread is in the
// process of aborting.
jsTestLog("Waiting for the index build to abort");
// Cleaned up index build after abort.
checkLog.containsJson(primary, 465611, {
    buildUUID: function(uuid) {
        return uuid && uuid["uuid"]["$uuid"] === extractUUIDFromObject(buildUUID);
    }
});

jsTestLog("Waiting for collection drop shell to return");
collDrop();
createIdx();

rst.stopSet();
})();
