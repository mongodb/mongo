/**
 * Tests that resumable index builds complete properly after being interrupted for rollback during
 * the collection scan phase.
 *
 * @tags: [
 *   requires_fcv_47,
 *   requires_majority_read_concern,
 *   requires_persistence,
 * ]
 */
(function() {
"use strict";

load('jstests/replsets/libs/rollback_resumable_index_build.js');

const dbName = "test";

const rollbackTest = new RollbackTest(jsTestName());

const runRollbackTo = function(rollbackEndFailPointName, rollbackEndFailPointLogIdWithBuildUUID) {
    const runTests = function(docs, indexSpecsFlat, collNameSuffix) {
        const runTest = function(indexSpecs) {
            RollbackResumableIndexBuildTest.run(
                rollbackTest,
                dbName,
                collNameSuffix,
                docs,
                indexSpecs,
                [{
                    name: "hangIndexBuildDuringCollectionScanPhaseBeforeInsertion",
                    logIdWithBuildUUID: 20386
                }],
                1,  // rollbackStartFailPointsIteration
                [{
                    name: rollbackEndFailPointName,
                    logIdWithBuildUUID: rollbackEndFailPointLogIdWithBuildUUID
                }],
                0,  // rollbackEndFailPointsIteration
                ["setYieldAllLocksHang", "setYieldAllLocksHangSecond"],
                ["collection scan"],
                [{numScannedAfterResume: 1}],
                [{a: 6}, {a: 7}]);
        };

        runTest([[indexSpecsFlat[0]]]);
        runTest([[indexSpecsFlat[0]], [indexSpecsFlat[1]]]);
        runTest([indexSpecsFlat]);
    };

    runTests([{a: 1, b: 1}, {a: 2, b: 2}, {a: 3, b: 3}], [{a: 1}, {b: 1}], "");
    runTests(
        [{a: [1, 2], b: [1, 2]}, {a: 2, b: 2}, {a: 3, b: 3}], [{a: 1}, {b: 1}], "_multikey_first");
    runTests(
        [{a: 1, b: 1}, {a: 2, b: 2}, {a: [3, 4], b: [3, 4]}], [{a: 1}, {b: 1}], "_multikey_last");
    runTests(
        [{a: [1, 2], b: 1}, {a: 2, b: 2}, {a: 3, b: [3, 4]}], [{a: 1}, {b: 1}], "_multikey_mixed");
    runTests([{a: [1, 2], b: {c: [3, 4]}, d: ""}, {e: "", f: [[]], g: null, h: 8}, {i: 9}],
             [{"$**": 1}, {h: 1}],
             "_wildcard");
};

// Rollback to before the indexes begin to be built.
runRollbackTo("hangAfterSettingUpIndexBuild", 20387);

// Rollback to earlier in the collection scan phase.
runRollbackTo("hangIndexBuildDuringCollectionScanPhaseAfterInsertion", 20386);

rollbackTest.stop();
})();
