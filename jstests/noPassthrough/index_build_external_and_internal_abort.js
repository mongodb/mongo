/**
 * Validate a scenario where an external index build abort (e.g. collection drop) races with an
 * internal index build abort (e.g. build failed due to invalid keys).
 *
 * @tags: [
 *   requires_fcv_71,
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

// Check external abort is reattempted multiple times, meaning it is blocked behind the internal
// abort.
assert.soon(() => checkLog.checkContainsWithAtLeastCountJson(primary, 4656010, {}, 3));

// Resume the index builder thread, which will transition to kAwaitPrimaryAbort and unblock external
// aborts.
indexBuilderThreadFP.off();

// Wait for completion.
awaitDropCollection();

waitForIndexBuild();

// The collection does not exist.
assert.eq(testDB.getCollectionNames().indexOf(coll.getName()), -1, "collection still exists.");

rst.stopSet();
})();
