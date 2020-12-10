/**
 * Tests that resumable index build state is written to disk upon clean shutdown when an index
 * build is in the drain writes phase on a primary, and that the index build is subsequently
 * completed when the node is started back up.
 *
 * @tags: [
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   requires_replication,
 *   sbe_incompatible,
 * ]
 */
(function() {
"use strict";

load("jstests/noPassthrough/libs/index_build.js");

const dbName = "test";
const collName = jsTestName();

const rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const coll = primary.getDB(dbName).getCollection(collName);

if (!ResumableIndexBuildTest.resumableIndexBuildsEnabled(primary)) {
    jsTestLog("Skipping test because resumable index builds are not enabled");
    rst.stopSet();
    return;
}

assert.commandWorked(coll.insert({a: 1}));

jsTestLog("Testing when primary shuts down in the middle of the first drain");

ResumableIndexBuildTest.run(
    rst,
    dbName,
    collName,
    [[{a: 1}]],
    [{name: "hangIndexBuildDuringDrainWritesPhase", logIdWithIndexName: 4841800}],
    0,
    ["drain writes"],
    [{skippedPhaseLogID: 20392}],
    [{a: 2}, {a: 3}],
    [{a: 4}, {a: 5}]);
ResumableIndexBuildTest.run(
    rst,
    dbName,
    collName,
    [[{a: 1}]],
    [{name: "hangIndexBuildDuringDrainWritesPhase", logIdWithIndexName: 4841800}],
    1,
    ["drain writes"],
    [{skippedPhaseLogID: 20392}],
    [{a: 6}, {a: 7}],
    [{a: 8}, {a: 9}]);

jsTestLog("Testing when primary shuts down after voting, but before commit quorum satisfied");

ResumableIndexBuildTest.runOnPrimaryToTestCommitQuorum(
    rst,
    dbName,
    collName,
    {a: 1},
    "hangIndexBuildAfterSignalPrimaryForCommitReadiness",
    "hangAfterIndexBuildFirstDrain",
    [{a: 10}, {a: 11}],
    [{a: 12}, {a: 13}]);

jsTestLog(
    "Testing when primary shuts down after commit quorum satisfied, but before commitIndexBuild oplog entry written");

ResumableIndexBuildTest.runOnPrimaryToTestCommitQuorum(
    rst,
    dbName,
    collName,
    {a: 1},
    "hangIndexBuildAfterSignalPrimaryForCommitReadiness",
    "hangIndexBuildAfterSignalPrimaryForCommitReadiness",
    [{a: 14}, {a: 15}],
    [{a: 16}, {a: 17}]);

rst.stopSet();
})();