/**
 * Confirms that aborting a background index build on a secondary during step up does not leave the
 * node in an inconsistent state.
 *
 * @tags: [
 *   # This test uses a failpoint that is only available for hybrid index builds.
 *   primary_driven_index_builds_incompatible,
 *   requires_replication,
 * ]
 */
import {configureFailPoint, kDefaultWaitForFailPointTimeout} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";

const rst = new ReplSetTest({
    nodes: [
        {},
        {
            slowms: 30000, // Don't log slow operations on secondary. See SERVER-44821.
        },
    ],
});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const testDB = primary.getDB("test");
const coll = testDB.getCollection("test");

assert.commandWorked(coll.insert({a: 1}));

let secondary = rst.getSecondary();
IndexBuildTest.pauseIndexBuilds(secondary);
let waitForCommitReadinessFP = configureFailPoint(primary, "hangIndexBuildAfterSignalPrimaryForCommitReadiness");

const awaitIndexBuild = IndexBuildTest.startIndexBuild(primary, coll.getFullName(), {a: 1}, {}, [
    ErrorCodes.InterruptedDueToReplStateChange,
]);

// When the index build starts, find its op id.
let secondaryDB = secondary.getDB(testDB.getName());
const opId = IndexBuildTest.waitForIndexBuildToStart(secondaryDB);

IndexBuildTest.assertIndexBuildCurrentOpContents(secondaryDB, opId, (op) => {
    jsTestLog("Inspecting db.currentOp() entry for index build: " + tojson(op));
    assert.eq(
        coll.getFullName(),
        op.ns,
        "Unexpected ns field value in db.currentOp() result for index build: " + tojson(op),
    );
});

// Step up the secondary and hang the process.
assert.commandWorked(secondary.adminCommand({configureFailPoint: "hangIndexBuildOnStepUp", mode: "alwaysOn"}));
// Wait for the index build to write the oplog entry indicating the primary is ready to commit.
waitForCommitReadinessFP.wait();
waitForCommitReadinessFP.off();
// Wait for replication to ensure the step up does not fail due to a lagged secondary.
rst.awaitReplication();
const awaitStepUp = startParallelShell(() => {
    assert.commandWorked(db.adminCommand({replSetStepUp: 1}));
}, secondary.port);

awaitIndexBuild();

assert.commandWorked(
    secondary.adminCommand({
        waitForFailPoint: "hangIndexBuildOnStepUp",
        timesEntered: 1,
        maxTimeMS: kDefaultWaitForFailPointTimeout,
    }),
);

// Kill the index build on the secondary as it is stepping up.
assert.commandWorked(secondaryDB.killOp(opId));

// Finish the step up.
assert.commandWorked(secondary.adminCommand({configureFailPoint: "hangIndexBuildOnStepUp", mode: "off"}));
rst.waitForState(secondary, ReplSetTest.State.PRIMARY);
awaitStepUp();

// Wait for the index build to be aborted before asserting that it doesn't exist.
IndexBuildTest.waitForIndexBuildToStop(secondaryDB, coll.getName(), "a_1");

const secondaryColl = secondaryDB.getCollection(coll.getName());
// Although the index is aborted on the secondary that's stepping up, we abort builds on secondaries
// (that is, we replicate 'abortIndexBuild') asynchronously wrt the index builder thread on the
// primary. Wait for the secondaries to complete the abort.
IndexBuildTest.assertIndexesSoon(coll, 1, ["_id_"]);
IndexBuildTest.assertIndexesSoon(secondaryColl, 1, ["_id_"]);

rst.stopSet();
