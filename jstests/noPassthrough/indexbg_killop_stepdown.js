/**
 * Confirms that aborting a background index build on a primary node during step down does not leave
 * the node in an inconsistent state.
 *
 * @tags: [requires_replication]
 */
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load('jstests/noPassthrough/libs/index_build.js');

const rst = new ReplSetTest({
    nodes: [
        {},
        {
            // Disallow elections on secondary.
            rsConfig: {
                priority: 0,
                votes: 0,
            },
        },
    ]
});
rst.startSet();
rst.initiate();

let primary = rst.getPrimary();
let testDB = primary.getDB('test');
let coll = testDB.getCollection('test');

assert.commandWorked(coll.insert({a: 1}));

IndexBuildTest.pauseIndexBuilds(primary);

const awaitIndexBuild =
    IndexBuildTest.startIndexBuild(primary, coll.getFullName(), {a: 1}, {background: true}, [
        ErrorCodes.InterruptedDueToReplStateChange
    ]);

// When the index build starts, find its op id.
const opId = IndexBuildTest.waitForIndexBuildToScanCollection(testDB, coll.getName(), 'a_1');

IndexBuildTest.assertIndexBuildCurrentOpContents(testDB, opId, (op) => {
    jsTestLog('Inspecting db.currentOp() entry for index build: ' + tojson(op));
    assert.eq(
        undefined,
        op.connectionId,
        'Was expecting IndexBuildsCoordinator op; found db.currentOp() for connection thread instead: ' +
            tojson(op));
    assert.eq(coll.getFullName(),
              op.ns,
              'Unexpected ns field value in db.currentOp() result for index build: ' + tojson(op));
});

// Index build should be present in the config.system.indexBuilds collection.
const indexMap =
    IndexBuildTest.assertIndexes(coll, 2, ["_id_"], ["a_1"], {includeBuildUUIDs: true});
const indexBuildUUID = indexMap['a_1'].buildUUID;
assert(primary.getCollection('config.system.indexBuilds').findOne({_id: indexBuildUUID}));

assert.commandWorked(primary.adminCommand(
    {configureFailPoint: "hangIndexBuildBeforeAbortCleanUp", mode: "alwaysOn"}));

// Signal the index builder thread to exit.
assert.commandWorked(testDB.killOp(opId));

// Wait for the index build to hang before cleaning up.
IndexBuildTest.resumeIndexBuilds(primary);
assert.commandWorked(primary.adminCommand({
    waitForFailPoint: "hangIndexBuildBeforeAbortCleanUp",
    timesEntered: 1,
    maxTimeMS: kDefaultWaitForFailPointTimeout
}));

// Step down the primary.
assert.commandWorked(testDB.adminCommand({"replSetStepDown": 5 * 60, "force": true}));
rst.waitForState(primary, ReplSetTest.State.SECONDARY);

awaitIndexBuild();

// Resume the abort, this should crash the node.
assert.commandWorked(
    primary.adminCommand({configureFailPoint: "hangIndexBuildBeforeAbortCleanUp", mode: "off"}));

assert.soon(function() {
    return rawMongoProgramOutput().search(/Fatal assertion.*51101/) >= 0;
});

// After restarting the old primary, we expect that the index build completes successfully.
const fassertProcessExitCode = _isWindows() ? MongoRunner.EXIT_ABRUPT : MongoRunner.EXIT_ABORT;
rst.stop(primary.nodeId, undefined, {forRestart: true, allowedExitCode: fassertProcessExitCode});
rst.start(primary.nodeId, undefined, true /* restart */);

// Wait for the index build to complete.
rst.awaitReplication();

// Verify that the stepped up node completed the index build.
IndexBuildTest.assertIndexes(
    rst.getPrimary().getDB('test').getCollection('test'), 2, ['_id_', 'a_1']);
IndexBuildTest.assertIndexes(
    rst.getSecondary().getDB('test').getCollection('test'), 2, ['_id_', 'a_1']);

rst.stopSet();
})();
