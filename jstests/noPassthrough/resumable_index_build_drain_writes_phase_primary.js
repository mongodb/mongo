/**
 * Tests that resumable index build state is written to disk upon clean shutdown when an index
 * build is in the drain writes phase on a primary, and that the index build is subsequently
 * completed when the node is started back up.
 *
 * @tags: [
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   requires_replication,
 * ]
 */
(function() {
"use strict";

load("jstests/noPassthrough/libs/index_build.js");
load("jstests/libs/sbe_util.js");  // For checkSBEEnabled.

const dbName = "test";
const collName = jsTestName();

const rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const coll = primary.getDB(dbName).getCollection(collName);

const columnstoreEnabled =
    checkSBEEnabled(primary.getDB(dbName), ["featureFlagColumnstoreIndexes"], true);

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

if (columnstoreEnabled) {
    ResumableIndexBuildTest.run(
        rst,
        dbName,
        collName,
        [[{"$**": "columnstore"}]],
        [{name: "hangIndexBuildDuringDrainWritesPhase", logIdWithIndexName: 4841800}],
        0,
        ["drain writes"],
        [{skippedPhaseLogID: 20392}],
        (function(collection) {
            assert.commandWorked(collection.insert([{a: [{b: 10}]}, {a: 11}]));
            assert.commandWorked(collection.update({a: 1}, {a: 2}));
            assert.commandWorked(collection.remove({"a.b": 10}));
            assert.commandWorked(collection.insert({a: 1}));
            assert.commandWorked(collection.remove({a: 1}));
            assert.commandWorked(collection.update({a: 11}, {a: 1}));
        }),
        [{a: 12}, {a: 13}]);
    ResumableIndexBuildTest.run(
        rst,
        dbName,
        collName,
        [[{"$**": "columnstore"}]],
        [{name: "hangIndexBuildDuringDrainWritesPhase", logIdWithIndexName: 4841800}],
        1,
        ["drain writes"],
        [{skippedPhaseLogID: 20392}],
        (function(collection) {
            assert.commandWorked(collection.insert([{a: [{b: 14}]}, {a: 15}]));
            assert.commandWorked(collection.update({a: 1}, {a: 2}));
            assert.commandWorked(collection.remove({"a.b": 14}));
            assert.commandWorked(collection.insert({a: 1}));
            assert.commandWorked(collection.remove({a: 1}));
            assert.commandWorked(collection.update({a: 15}, {a: 1}));
        }),
        [{a: 16}, {a: 17}]);

    jsTestLog("Testing when primary shuts down after voting, but before commit quorum satisfied");

    ResumableIndexBuildTest.runOnPrimaryToTestCommitQuorum(
        rst,
        dbName,
        collName,
        {"$**": "columnstore"},
        "hangIndexBuildAfterSignalPrimaryForCommitReadiness",
        "hangAfterIndexBuildFirstDrain",
        (function(collection) {
            assert.commandWorked(collection.insert([{a: [{b: 22}]}, {a: 23}]));
            assert.commandWorked(collection.update({a: 1}, {a: 2}));
            assert.commandWorked(collection.remove({"a.b": 22}));
            assert.commandWorked(collection.insert({a: 1}));
            assert.commandWorked(collection.remove({a: 1}));
            assert.commandWorked(collection.update({a: 23}, {a: 1}));
        }),
        [{a: 24}, {a: 25}]);

    jsTestLog(
        "Testing when primary shuts down after commit quorum satisfied, but before commitIndexBuild oplog entry written");

    ResumableIndexBuildTest.runOnPrimaryToTestCommitQuorum(
        rst,
        dbName,
        collName,
        {"$**": "columnstore"},
        "hangIndexBuildAfterSignalPrimaryForCommitReadiness",
        "hangIndexBuildAfterSignalPrimaryForCommitReadiness",
        (function(collection) {
            assert.commandWorked(collection.insert([{a: [{b: 30}]}, {a: 31}]));
            assert.commandWorked(collection.update({a: 1}, {a: 2}));
            assert.commandWorked(collection.remove({"a.b": 30}));
            assert.commandWorked(collection.insert({a: 1}));
            assert.commandWorked(collection.remove({a: 1}));
            assert.commandWorked(collection.update({a: 31}, {a: 1}));
        }),
        [{a: 32}, {a: 33}]);
}

rst.stopSet();
})();
