/**
 * Tests that resumable index build state is written to disk upon clean shutdown when an index
 * build is in the collection scan phase, and that the index build is subsequently completed when
 * the node is started back up.
 *
 * @tags: [requires_persistence, requires_replication]
 */
(function() {
"use strict";

load("jstests/noPassthrough/libs/index_build.js");

const dbName = "test";
const collName = "resumable_index_build_collection_scan_phase";

const rst = new ReplSetTest({nodes: 1});
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
assert.commandWorked(coll.insert({a: 2}));

ResumableIndexBuildTest.run(
    rst, dbName, collName, {a: 1}, "hangIndexBuildDuringCollectionScanPhaseBeforeInsertion", {
        fieldsToMatch: {a: 1}
    });
ResumableIndexBuildTest.run(
    rst, dbName, collName, {a: 1}, "hangIndexBuildDuringCollectionScanPhaseBeforeInsertion", {
        fieldsToMatch: {a: 2}
    });

rst.stopSet();
})();