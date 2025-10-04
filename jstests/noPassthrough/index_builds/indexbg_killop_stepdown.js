/**
 * Confirms that aborting a background index build on a primary node during step down does not leave
 * the node in an inconsistent state.
 *
 * @tags: [
 *   # Tests the hybrid index build-specific invalid state transition which involves an index on the secondary being aborted
 *   # while the primary commits.
 *   primary_driven_index_builds_incompatible,
 *   requires_replication,
 * ]
 */
import {kDefaultWaitForFailPointTimeout} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";

const rst = new ReplSetTest({nodes: 3});
rst.startSet();
rst.initiate();

let primary = rst.getPrimary();
let testDB = primary.getDB("test");
let coll = testDB.getCollection("test");

assert.commandWorked(coll.insert({a: 1}));

IndexBuildTest.pauseIndexBuilds(primary);

const awaitIndexBuild = IndexBuildTest.startIndexBuild(primary, coll.getFullName(), {a: 1}, {}, [
    ErrorCodes.InterruptedDueToReplStateChange,
]);

// When the index build starts, find its op id.
const opId = IndexBuildTest.waitForIndexBuildToScanCollection(testDB, coll.getName(), "a_1");

IndexBuildTest.assertIndexBuildCurrentOpContents(testDB, opId, (op) => {
    jsTestLog("Inspecting db.currentOp() entry for index build: " + tojson(op));
    assert.eq(
        undefined,
        op.connectionId,
        "Was expecting IndexBuildsCoordinator op; found db.currentOp() for connection thread instead: " + tojson(op),
    );
    assert.eq(
        coll.getFullName(),
        op.ns,
        "Unexpected ns field value in db.currentOp() result for index build: " + tojson(op),
    );
});

// Index build should be present in the config.system.indexBuilds collection.
const indexMap = IndexBuildTest.assertIndexes(coll, 2, ["_id_"], ["a_1"], {includeBuildUUIDs: true});
const indexBuildUUID = indexMap["a_1"].buildUUID;
assert(primary.getCollection("config.system.indexBuilds").findOne({_id: indexBuildUUID}));

assert.commandWorked(primary.adminCommand({configureFailPoint: "hangIndexBuildBeforeAbortCleanUp", mode: "alwaysOn"}));

// Signal the index builder thread to exit.
assert.commandWorked(testDB.killOp(opId));

// Wait for the index build to hang before cleaning up.
IndexBuildTest.resumeIndexBuilds(primary);
assert.commandWorked(
    primary.adminCommand({
        waitForFailPoint: "hangIndexBuildBeforeAbortCleanUp",
        timesEntered: 1,
        maxTimeMS: kDefaultWaitForFailPointTimeout,
    }),
);

// Step down the primary.
assert.commandWorked(testDB.adminCommand({replSetStepDown: 10, secondaryCatchUpPeriodSecs: 10}));
rst.awaitSecondaryNodes(null, [primary]);

// Resume the abort.
assert.commandWorked(primary.adminCommand({configureFailPoint: "hangIndexBuildBeforeAbortCleanUp", mode: "off"}));

awaitIndexBuild();

// Wait for primary and secondaries to reach goal state, and for the index build to complete.
primary = rst.waitForPrimary();
rst.awaitSecondaryNodes();
rst.awaitReplication();

// "Index build: joined after abort".
checkLog.containsJson(primary, 20655);

// Wait for the index build to complete.
rst.awaitReplication();

// Verify that the interrupted index build was aborted.
IndexBuildTest.assertIndexes(rst.getPrimary().getDB("test").getCollection("test"), 1, ["_id_"]);
IndexBuildTest.assertIndexes(rst.getSecondary().getDB("test").getCollection("test"), 1, ["_id_"]);

rst.stopSet();
