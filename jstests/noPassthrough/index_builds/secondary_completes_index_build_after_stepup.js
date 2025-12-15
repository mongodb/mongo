/**
 * Tests that if secondaries have voted but the primary has not, and if a secondary steps up and sees
 * that commit quorum is satisfied, it proceeds to commit the index build.
 *
 * @tags: [
 *   requires_commit_quorum,
 *   requires_replication,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";

const rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const testDB = primary.getDB("test");
const coll = testDB.getCollection("test");

assert.commandWorked(coll.insert({a: 1}));

const secondary = rst.getSecondary();
const secondaryDB = secondary.getDB(testDB.getName());
const secondaryColl = secondaryDB.getCollection(coll.getName());

// Pause primary index build after starting.
IndexBuildTest.pauseIndexBuilds(primary);

jsTest.log.info("Waiting for index build to start");
const createIdx = IndexBuildTest.startIndexBuild(
    primary,
    coll.getFullName(),
    {a: 1},
    null,
    /* expectedFailures */ [ErrorCodes.InterruptedDueToReplStateChange],
    /* commitQuorum */ 1,
);

// Wait for the index build to start on both nodes.
const opId = IndexBuildTest.waitForIndexBuildToStart(testDB, coll.getName(), "a_1");
IndexBuildTest.assertIndexBuildCurrentOpContents(testDB, opId);
IndexBuildTest.assertIndexesSoon(coll, 2, ["_id_", "a_1"]);

const secondaryOpId = IndexBuildTest.waitForIndexBuildToStart(secondaryDB, coll.getName(), "a_1");
IndexBuildTest.assertIndexBuildCurrentOpContents(secondaryDB, secondaryOpId);
IndexBuildTest.assertIndexesSoon(secondaryColl, 2, ["_id_", "a_1"]);

// Before stepping down primary, make sure secondary pauses on step up.
const hangOnStepUpAsyncTaskBeforeCheckingCommitQuorum = configureFailPoint(
    secondaryDB,
    "hangOnStepUpAsyncTaskBeforeCheckingCommitQuorum",
);

jsTest.log.info("Waiting for primary to step down");
rst.awaitReplication();
const stepDown = startParallelShell(() => {
    assert.commandWorked(db.adminCommand({"replSetStepDown": 60, "force": false}));
}, primary.port);
// Wait for stepdown to complete.
stepDown();

// The index build on old primary will continue in the background.
const exitCode = createIdx();
assert.eq(0, exitCode, "expected shell to exit successfully");

jsTest.log.info("Waiting for secondary to step up and satisfy commit quorum as new primary");
hangOnStepUpAsyncTaskBeforeCheckingCommitQuorum.wait();

// Resume index builds on both nodes.
IndexBuildTest.resumeIndexBuilds(primary);
hangOnStepUpAsyncTaskBeforeCheckingCommitQuorum.off();

jsTest.log.info("Waiting for index build to stop");
IndexBuildTest.waitForIndexBuildToStop(testDB);
IndexBuildTest.waitForIndexBuildToStop(secondaryDB);

// Expect "Index build: completed successfully" in the log.
checkLog.containsJson(primary, 20663, {
    namespace: coll.getFullName(),
    indexesBuilt: ["a_1"],
    numIndexesAfter: 2,
});
checkLog.containsJson(secondary, 20663, {
    namespace: coll.getFullName(),
    indexesBuilt: ["a_1"],
    numIndexesAfter: 2,
});

rst.stopSet();
