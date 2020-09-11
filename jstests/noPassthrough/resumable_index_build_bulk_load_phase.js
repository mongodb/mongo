/**
 * Tests that resumable index build state is written to disk upon clean shutdown when an index
 * build is in the bulk load phase, and that the index build is subsequently completed when the
 * node is started back up.
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

// TODO (SERVER-50920): Re-enable once resuming from the bulk load phase is fixed.
if (true) {
    jsTestLog("Skipping test.");
    return;
}

const dbName = "test";
const failPointName = "hangIndexBuildDuringBulkLoadPhase";

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const runTests = function(docs, indexSpec, collNameSuffix) {
    const coll = rst.getPrimary().getDB(dbName).getCollection(jsTestName() + collNameSuffix);
    assert.commandWorked(coll.insert(docs));

    const runTest = function(iteration) {
        ResumableIndexBuildTest.run(rst,
                                    dbName,
                                    coll.getName(),
                                    indexSpec,
                                    failPointName,
                                    {iteration: iteration},
                                    "bulk load",
                                    {skippedPhaseLogID: 20391});
    };

    runTest(0);
    runTest(1);
};

runTests([{a: 1}, {a: 2}], {a: 1}, "");
runTests([{a: [1, 2]}, {a: 2}], {a: 1}, "_multikey_first");
runTests([{a: 1}, {a: [1, 2]}], {a: 1}, "_multikey_last");
runTests(
    [{a: [1, 2], b: {c: [3, 4]}, d: ""}, {e: "", f: [[]], g: null, h: 8}], {"$**": 1}, "_wildcard");

rst.stopSet();
})();