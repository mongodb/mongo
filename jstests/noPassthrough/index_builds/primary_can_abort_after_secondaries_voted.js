/**
 * Tests that even when enough secondaries have voted to commit an index build, the primary does
 * not consider commit quorum satisfied if itself has not completed. The index build can still
 * be aborted on the primary instead of hanging indefinitely.
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
// Pause secondary index build after voting for commit.
const hangAfterVoteCommit = configureFailPoint(secondaryDB, "hangIndexBuildAfterSignalPrimaryForCommitReadiness");

jsTest.log.info("Waiting for index build to start");
const createIdx = IndexBuildTest.startIndexBuild(
    primary,
    coll.getFullName(),
    {a: 1},
    null,
    /* expectedFailures */ [ErrorCodes.Interrupted],
    /* commitQuorum */ 1,
);

// Wait for the index build to start on both nodes.
const opId = IndexBuildTest.waitForIndexBuildToStart(testDB, coll.getName(), "a_1");
IndexBuildTest.assertIndexBuildCurrentOpContents(testDB, opId);
const secondaryOpId = IndexBuildTest.waitForIndexBuildToStart(secondaryDB, coll.getName(), "a_1");
IndexBuildTest.assertIndexBuildCurrentOpContents(secondaryDB, secondaryOpId);

jsTest.log.info("Waiting for secondary to vote to commit the index");
hangAfterVoteCommit.wait();
IndexBuildTest.assertIndexesSoon(secondaryColl, 2, ["_id_", "a_1"]);

// Primary should not consider commit quorum satisfied and still allow to abort.
IndexBuildTest.assertIndexesSoon(coll, 2, ["_id_", "a_1"]);
testDB.killOp(opId);

jsTest.log.info("Waiting for index build to stop");
IndexBuildTest.waitForIndexBuildToStop(testDB);
IndexBuildTest.assertIndexesSoon(coll, 1, ["_id_"]);

IndexBuildTest.waitForIndexBuildToStop(secondaryDB);
IndexBuildTest.assertIndexesSoon(secondaryColl, 1, ["_id_"]);

const exitCode = createIdx();
assert.eq(0, exitCode, "expected shell to exit successfully");

rst.stopSet();
