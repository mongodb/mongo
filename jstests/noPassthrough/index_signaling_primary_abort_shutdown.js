/**
 * Tests that a failing index build on a secondary node which is trying to signal the primary, is
 * properly interrupted, without blocking shutdown, and restarted after shutdown.
 *
 * @tags: [
 *   requires_fcv_71,
 *   requires_replication,
 * ]
 */
(function() {
"use strict";

load('jstests/noPassthrough/libs/index_build.js');
load('jstests/libs/fail_point_util.js');

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

const testDB = primary.getDB('test');
const coll = testDB.getCollection('test');

const secondary = rst.getSecondary();
const secondaryDB = secondary.getDB(testDB.getName());
let secondaryColl = secondaryDB.getCollection('test');

// Avoid optimization on empty colls.
assert.commandWorked(coll.insert({a: 1}));

// Pause the index builds on the secondary, using the 'hangAfterStartingIndexBuild' failpoint.
const failpointHangAfterInit = configureFailPoint(secondaryDB, "hangAfterInitializingIndexBuild");

// Create the index and start the build. Set commitQuorum of 2 nodes explicitly, otherwise as only
// primary is voter, it would immediately commit.
const createIdx = IndexBuildTest.startIndexBuild(primary,
                                                 coll.getFullName(),
                                                 {a: 1},
                                                 {},
                                                 [ErrorCodes.InterruptedDueToReplStateChange],
                                                 /*commitQuorum: */ 2);

failpointHangAfterInit.wait();

// Extract the index build UUID. Use assertIndexesSoon to retry until the oplog applier is done with
// the entry, and the index is visible to listIndexes. The failpoint does not ensure this.
const buildUUID =
    IndexBuildTest
        .assertIndexesSoon(secondaryColl, 2, ['_id_'], ['a_1'], {includeBuildUUIDs: true})['a_1']
        .buildUUID;

const hangBeforePrimarySignal =
    configureFailPoint(secondaryDB, "hangIndexBuildBeforeSignalingPrimaryForAbort");
const failSecondaryBuild =
    configureFailPoint(secondaryDB,
                       "failIndexBuildWithError",
                       {buildUUID: buildUUID, error: ErrorCodes.OutOfDiskSpace});

// Unblock index builds, causing the failIndexBuildWithError failpoint to throw an error.
failpointHangAfterInit.off();
hangBeforePrimarySignal.wait();

// Restarting the secondary while voting causes it to be interrupted, even then it should mark the
// build as aborted for resume and allow shutdown to finish.
rst.restart(secondary);
rst.waitForPrimary();

// Should exit normally, because after restart the error failpoint is not set. The index build
// should be restarted, and vote commit to the primary. InterruptedDueToReplStateChange is ignored,
// as a slow secondary restart can cause the primary to step down due to heartbeat timeout.
createIdx();

secondaryColl = rst.getSecondary().getDB(testDB.getName()).getCollection('test');

// Wait for index build to finish.
IndexBuildTest.waitForIndexBuildToStop(testDB);

// Assert index is committed.
IndexBuildTest.assertIndexes(coll, 2, ['_id_', 'a_1'], []);
IndexBuildTest.assertIndexes(secondaryColl, 2, ['_id_', 'a_1'], []);

rst.stopSet();
})();
