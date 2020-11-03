/**
 * Tests that resumable index builds complete properly after being interrupted for rollback during
 * the drain writes phase.
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

const runRollbackTo = function(rollbackEndFailPoint) {
    const runTests = function(sideWrites, indexSpecsFlat, collNameSuffix) {
        const runTest = function(indexSpecs) {
            RollbackResumableIndexBuildTest.run(
                rollbackTest,
                dbName,
                collNameSuffix,
                [{a: 10, b: 10}, {a: 11, b: 11}, {a: 12, b: 12}],
                indexSpecs,
                [{name: "hangIndexBuildDuringDrainWritesPhase", logIdWithIndexName: 4841800}],
                3,  // rollbackStartFailPointsIteration
                [rollbackEndFailPoint],
                1,  // rollbackEndFailPointsIteration
                ["hangDuringIndexBuildDrainYield", "hangDuringIndexBuildDrainYieldSecond"],
                ["drain writes"],
                [{skippedPhaseLogID: 20392}],
                [{a: 13, b: 13}, {a: 14, b: 14}],
                sideWrites);
        };

        runTest([[indexSpecsFlat[0]]]);
        runTest([[indexSpecsFlat[0]], [indexSpecsFlat[1]]]);
        runTest([indexSpecsFlat]);
    };

    runTests([{a: 1, b: 1}, {a: 2, b: 2}, {a: 3, b: 3}, {a: 4, b: 4}, {a: 5, b: 5}],
             [{a: 1}, {b: 1}],
             "");
    runTests([{a: [1, 2], b: [1, 2]}, {a: 2, b: 2}, {a: 3, b: 3}, {a: 4, b: 4}, {a: 5, b: 5}],
             [{a: 1}, {b: 1}],
             "_multikey_first");
    runTests([{a: 1, b: 1}, {a: 2, b: 2}, {a: 3, b: 3}, {a: 4, b: 4}, {a: [5, 6], b: [5, 6]}],
             [{a: 1}, {b: 1}],
             "_multikey_last");
    runTests([{a: [1, 2], b: 1}, {a: 2, b: 2}, {a: 3, b: 3}, {a: 4, b: 4}, {a: 5, b: [5, 6]}],
             [{a: 1}, {b: 1}],
             "_multikey_mixed");
    runTests(
        [
            {a: [1, 2], b: {c: [3, 4]}},
            {d: "", e: ""},
            {f: [[]], g: null},
            {h: 8, i: 9},
            {j: [{}], k: {}}
        ],
        [{"$**": 1}, {h: 1}],
        "_wildcard");
};

// Rollback to before the indexes begin to be built.
runRollbackTo({name: "hangAfterSettingUpIndexBuild", logIdWithBuildUUID: 20387});

// Rollback to the collection scan phase.
runRollbackTo(
    {name: "hangIndexBuildDuringCollectionScanPhaseBeforeInsertion", logIdWithBuildUUID: 20386});

// Rollback to the bulk load phase.
runRollbackTo({name: "hangIndexBuildDuringBulkLoadPhase", logIdWithIndexName: 4924400});

// Rollback to earlier in the drain writes phase.
runRollbackTo({name: "hangIndexBuildDuringDrainWritesPhaseSecond", logIdWithIndexName: 4841800});

rollbackTest.stop();
})();
