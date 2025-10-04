/**
 * Tests a race condition between a user interrupting an index build and the node stepping-down. The
 * nature of this problem is that the stepping-down node is not able to replicate an abortIndexBuild
 * oplog entry after the user kills the operation. The old primary will rely on the new primary to
 * replicate a commitIndexBuild oplog entry after the takeover.
 *
 * @tags: [
 *   requires_replication,
 * ]
 */
import {kDefaultWaitForFailPointTimeout} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";

const rst = new ReplSetTest({
    nodes: [{}, {}],
});
rst.startSet();
rst.initiate(null, null, {initiateWithDefaultElectionTimeout: true});

const primary = rst.getPrimary();
const testDB = primary.getDB("test");
const coll = testDB.getCollection("test");

assert.commandWorked(coll.insert({a: 1}));

let res = assert.commandWorked(
    primary.adminCommand({configureFailPoint: "hangBeforeIndexBuildAbortOnInterrupt", mode: "alwaysOn"}),
);
const hangBeforeAbortFailpointTimesEntered = res.count;

IndexBuildTest.pauseIndexBuilds(primary);
const createIdx = IndexBuildTest.startIndexBuild(primary, coll.getFullName(), {a: 1}, {}, [ErrorCodes.Interrupted]);

// When the index build starts, find its op id. This will be the op id of the client connection, not
// the thread pool task managed by IndexBuildsCoordinatorMongod.
const filter = {
    "desc": {$regex: /conn.*/},
};
const opId = IndexBuildTest.waitForIndexBuildToStart(testDB, coll.getName(), "a_1", filter);

// Kill the index build.
assert.commandWorked(testDB.killOp(opId));

// Wait for the command thread to observe the killOp, or quit early if the index build was killed
// prematurely, escaping the failpoint forever. Retry up to 10 times.
let retry = 0;
while (
    assert.commandWorkedOrFailedWithCode(
        primary.adminCommand({
            waitForFailPoint: "hangBeforeIndexBuildAbortOnInterrupt",
            timesEntered: hangBeforeAbortFailpointTimesEntered + 1,
            maxTimeMS: kDefaultWaitForFailPointTimeout / 10,
        }),
        ErrorCodes.MaxTimeMSExpired,
    ).code == ErrorCodes.MaxTimeMSExpired
) {
    if (IndexBuildTest.getIndexBuildOpId(testDB, coll.getName(), "a_1") === -1) {
        jsTestLog("Index build killed too early, exiting.");
        quit();
    }
    assert.lt(++retry, 10, "waitForFailPoint hangBeforeIndexBuildAbortOnInterrupt timed out");
}

// Step down the primary, preventing the index build from generating an abort oplog entry.
assert.commandWorked(testDB.adminCommand({replSetStepDown: 30, force: true}));

// Let the command thread try to abort the index build.
assert.commandWorked(primary.adminCommand({configureFailPoint: "hangBeforeIndexBuildAbortOnInterrupt", mode: "off"}));

// Unable to abort index build because we are not primary.
checkLog.containsJson(primary, 20449);

createIdx();

// Let the index build continue running.
IndexBuildTest.resumeIndexBuilds(primary);

// Wait for the index build to stop.
IndexBuildTest.waitForIndexBuildToStop(testDB);

// With two phase index builds, a stepdown will not abort the index build, which should complete
// after a new node becomes primary.
rst.awaitReplication();

// The old primary, now secondary, should process the commitIndexBuild oplog entry.

const secondaryColl = rst.getSecondary().getCollection(coll.getFullName());
IndexBuildTest.assertIndexes(coll, 2, ["_id_", "a_1"], [], {includeBuildUUIDs: true});
IndexBuildTest.assertIndexes(secondaryColl, 2, ["_id_", "a_1"], [], {includeBuildUUIDs: true});

rst.stopSet();
