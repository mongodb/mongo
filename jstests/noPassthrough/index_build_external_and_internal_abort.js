/**
 * Validate a scenario where an external index build abort (e.g. collection drop) races with an
 * internal index build abort (e.g. build failed due to invalid keys).
 *
 * @tags: [
 *   featureFlagIndexBuildGracefulErrorHandling,
 *   requires_replication,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/feature_flag_util.js");
load('jstests/noPassthrough/libs/index_build.js');

const rst = new ReplSetTest({
    nodes: [
        {},
    ]
});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const testDB = primary.getDB('test');
const coll = testDB.getCollection('test');

assert.commandWorked(coll.insert({point: {x: -15.0, y: "abc"}}));

let indexBuilderThreadFP =
    configureFailPoint(testDB, 'hangIndexBuildBeforeTransitioningReplStateTokAwaitPrimaryAbort');
let connThreadFP = configureFailPoint(testDB, 'hangInRemoveIndexBuildEntryAfterCommitOrAbort');

// Will fail with error code 13026: "geo values must be 'legacy coordinate pairs' for 2d indexes"
const waitForIndexBuild =
    IndexBuildTest.startIndexBuild(primary, coll.getFullName(), {point: "2d"}, {}, 13026);

// Wait for the index builder to detect the malformed geo value and to hang before transitioning
// the index build state to kAwaitPrimaryAbort.
indexBuilderThreadFP.wait();

// Drop the underlying collection.
const awaitDropCollection =
    startParallelShell(funWithArgs(function(collName) {
                           assert.commandWorked(db.runCommand({drop: collName}));
                       }, coll.getName()), primary.port);

// Wait for the 'drop' command to hang while tearing down the index build, just after setting the
// index build state to kAborted.
connThreadFP.wait();

// Resume the index builder thread, which would now try to abort an index that's already in kAbort
// state.
indexBuilderThreadFP.off();

// Wait for the log to confirm the index builder won't attempt to abort the build, because it's
// already in aborted state.
checkLog.containsJson(primary, 7530800);

// Resume the collection drop and wait for its completion.
connThreadFP.off();
awaitDropCollection();

waitForIndexBuild();

// The collection does not exist.
assert.eq(testDB.getCollectionNames().indexOf(coll.getName()), -1, "collection still exists.");

rst.stopSet();
})();
