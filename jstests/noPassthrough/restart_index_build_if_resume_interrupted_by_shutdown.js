/**
 * Tests that an index build is resumable only once across restarts. If the resumed index build
 * fails to run to completion before shutdown, it will restart from the beginning on the next server
 * startup.
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
load("jstests/libs/sbe_util.js");          // For checkSBEEnabled.
load("jstests/libs/columnstore_util.js");  // For setUpServerForColumnStoreIndexTest.

const dbName = "test";
const collName = jsTestName();

let rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

let primary = rst.getPrimary();
const columnstoreEnabled =
    checkSBEEnabled(
        primary.getDB(dbName), ["featureFlagColumnstoreIndexes", "featureFlagSbeFull"], true) &&
    setUpServerForColumnStoreIndexTest(primary.getDB(dbName));

ResumableIndexBuildTest.runResumeInterruptedByShutdown(
    rst,
    dbName,
    collName + "_collscan_drain",
    {a: 1},                    // index key pattern
    "resumable_index_build1",  // index name
    {name: "hangIndexBuildDuringCollectionScanPhaseBeforeInsertion", logIdWithBuildUUID: 20386},
    "collection scan",
    {a: 1},  // initial doc
    [{a: 2}, {a: 3}],
    [{a: 4}, {a: 5}]);

ResumableIndexBuildTest.runResumeInterruptedByShutdown(
    rst,
    dbName,
    collName + "_bulkload_drain_multikey",
    {a: 1},                    // index key pattern
    "resumable_index_build2",  // index name
    {name: "hangIndexBuildDuringBulkLoadPhase", logIdWithIndexName: 4924400},
    "bulk load",
    {a: [11, 22, 33]},  // initial doc
    [{a: 77}, {a: 88}],
    [{a: 99}, {a: 100}]);

if (columnstoreEnabled) {
    ResumableIndexBuildTest.runResumeInterruptedByShutdown(
        rst,
        dbName,
        collName + "_collscan_drain_column_store",
        {"$**": "columnstore"},    // index key pattern
        "resumable_index_build3",  // index name
        {name: "hangIndexBuildDuringCollectionScanPhaseBeforeInsertion", logIdWithBuildUUID: 20386},
        "collection scan",
        {a: 1},  // initial doc
        (function(collection) {
            assert.commandWorked(collection.insert([{a: [{b: 14}]}, {a: 15}]));
            assert.commandWorked(collection.update({a: 1}, {a: 2}));
            assert.commandWorked(collection.remove({"a.b": 14}));
            assert.commandWorked(collection.insert({a: 1}));
            assert.commandWorked(collection.remove({a: 2}));
            assert.commandWorked(collection.update({a: 15}, {a: 2}));
        }),
        [{a: 16}, {a: 17}]);

    ResumableIndexBuildTest.runResumeInterruptedByShutdown(
        rst,
        dbName,
        collName + "_bulkload_drain_column_store",
        {"$**": "columnstore"},    // index key pattern
        "resumable_index_build4",  // index name
        {name: "hangIndexBuildDuringBulkLoadPhase", logIdWithIndexName: 4924400},
        "bulk load",
        {a: [44, 55, 66]},  // initial doc
        (function(collection) {
            assert.commandWorked(collection.insert([{a: [{b: 77}]}, {a: 88}]));
            assert.commandWorked(collection.update({a: [44, 55, 66]}, {a: [55, 66]}));
            assert.commandWorked(collection.remove({"a.b": 77}));
            assert.commandWorked(collection.insert({a: 99}));
            assert.commandWorked(collection.remove({a: [55, 66]}));
            assert.commandWorked(collection.update({a: 99}, {a: 1}));
        }),
        [{a: 99}, {a: 100}]);
}
rst.stopSet();
})();
