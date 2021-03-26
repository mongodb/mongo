/**
 * Tests that resumable index builds in the drain writes phase write their state to disk upon clean
 * shutdown and are resumed from the same phase to completion when the node is started back up.
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

const dbName = "test";

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const runTests = function(docs, indexSpecsFlat, sideWrites, collNameSuffix) {
    const coll = rst.getPrimary().getDB(dbName).getCollection(jsTestName() + collNameSuffix);
    assert.commandWorked(coll.insert(docs));

    const runTest = function(indexSpecs, iteration) {
        ResumableIndexBuildTest.run(
            rst,
            dbName,
            coll.getName(),
            indexSpecs,
            [{name: "hangIndexBuildDuringDrainWritesPhase", logIdWithIndexName: 4841800}],
            iteration,
            ["drain writes"],
            [{skippedPhaseLogID: 20392}],
            sideWrites,
            [{a: 4}, {a: 5}]);
    };

    runTest([[indexSpecsFlat[0]]], 0);
    runTest([[indexSpecsFlat[0]]], 1);
    runTest([[indexSpecsFlat[0]], [indexSpecsFlat[1]]], 0);
    runTest([[indexSpecsFlat[0]], [indexSpecsFlat[1]]], 1);
    runTest([indexSpecsFlat], 0);
    runTest([indexSpecsFlat], 1);
};

runTests({a: 1}, [{a: 1}, {b: 1}], [{a: 2, b: 2}, {a: 3, b: 3}], "");
runTests({a: 1}, [{a: 1}, {b: 1}], [{a: [2, 3], b: [2, 3]}, {a: 3, b: 3}], "_multikey_first");
runTests({a: 1}, [{a: 1}, {b: 1}], [{a: 2, b: 2}, {a: [3, 4], b: [3, 4]}], "_multikey_last");
runTests({a: 1}, [{a: 1}, {b: 1}], [{a: [2, 3], b: 2}, {a: 3, b: [3, 4]}], "_multikey_mixed");
runTests({a: 1},
         [{"$**": 1}, {h: 1}],
         [{a: [1, 2], b: {c: [3, 4]}, d: ""}, {e: "", f: [[]], g: null, h: 8}],
         "_wildcard");

rst.stopSet();
})();