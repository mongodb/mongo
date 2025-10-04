/**
 * Verifies that the index build step-up async task handles a stepdown gracefully.
 *
 * @tags: [
 *   # TODO(SERVER-109702): Evaluate if a primary-driven index build compatible test should be created.
 *   requires_commit_quorum,
 *   requires_fcv_71,
 *   requires_replication,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";

const rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate(null, null, {initiateWithDefaultElectionTimeout: true});

const dbName = "test";
const collName = "coll";
const primary = rst.getPrimary();
const primaryDB = primary.getDB(dbName);
const primaryColl = primaryDB.getCollection(collName);

assert.commandWorked(primaryColl.insert({a: 1}));

rst.awaitReplication();

const secondary = rst.getSecondary();

const hangAfterIndexBuildDumpsInsertsFromBulk = configureFailPoint(primary, "hangAfterIndexBuildDumpsInsertsFromBulk");
const hangOnStepUpAsyncTaskBeforeCheckingCommitQuorum = configureFailPoint(
    secondary,
    "hangOnStepUpAsyncTaskBeforeCheckingCommitQuorum",
);

const waitForIndexBuildToComplete = IndexBuildTest.startIndexBuild(primary, primaryColl.getFullName(), {a: 1}, null, [
    ErrorCodes.InterruptedDueToReplStateChange,
]);

// Wait for the primary to start the index build.
hangAfterIndexBuildDumpsInsertsFromBulk.wait();

assert.commandWorked(primary.adminCommand({replSetStepDown: 60, force: true}));

// The old secondary is now stepping up and checking the active index builds.
// "IndexBuildsCoordinator-StepUp [..] Active index builds"
hangOnStepUpAsyncTaskBeforeCheckingCommitQuorum.wait();
checkLog.containsJson(secondary, 20650);

// Step down the new primary.
const waitForStepDown = startParallelShell(() => {
    assert.commandWorked(db.adminCommand({replSetStepDown: 60 * 60, force: true}));
}, secondary.port);

// Wait for the RstlKillOpThread to run again. It first ran when the secondary stepped up (earlier
// in this test case), and it's running now when it's stepping down again.
assert.soon(() => {
    // Check both the RSTL kill op thread and the Intent Registry kill op thread.
    return (
        checkLog.checkContainsWithCountJson(secondary, 21343, {}, 2) ||
        checkLog.checkContainsWithCountJson(secondary, 9945003, {}, 2)
    );
});

// Wait for the step-up task to be marked as killPending by the RstlKillOpThread.
assert.soon(() => {
    return (
        1 ===
        secondary.getDB("test").currentOp({desc: "IndexBuildsCoordinator-StepUp", killPending: true})["inprog"].length
    );
});

// Turn off the failpoints. Allow the createIndexes command to return
// InterruptedDueToReplStateChange due to stepdown, the stepped-up secondary to complete the new
// stepdown, and the index build to succeed.
hangOnStepUpAsyncTaskBeforeCheckingCommitQuorum.off();
hangAfterIndexBuildDumpsInsertsFromBulk.off();
waitForIndexBuildToComplete();
waitForStepDown();

IndexBuildTest.assertIndexesSoon(rst.getPrimary().getDB(dbName).getCollection(collName), 2, ["_id_", "a_1"]);
IndexBuildTest.assertIndexesSoon(rst.getSecondary().getDB(dbName).getCollection(collName), 2, ["_id_", "a_1"]);

rst.stopSet();
