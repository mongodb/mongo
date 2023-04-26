/**
 * Tests that resumable index builds complete properly after being interrupted for rollback in
 * different phases.
 *
 * @tags: [
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   # The rollback can be slow on certain build variants (such as macOS and code coverage), which
 *   # can cause the targeted log messages to fall off the log buffer before we search for them.
 *   incompatible_with_gcov,
 * ]
 */
(function() {
"use strict";

load('jstests/replsets/libs/rollback_resumable_index_build.js');

const dbName = "test";
const rollbackTest = new RollbackTest(jsTestName());

const runRollbackTo = function(rollbackStartFailPoints,
                               rollbackEndFailPoints,
                               locksYieldedFailPoints,
                               resumePhases,
                               resumeChecks) {
    const runTest = function(docs, sideWrites, indexSpecsFlat, collNameSuffix) {
        RollbackResumableIndexBuildTest.run(rollbackTest,
                                            dbName,
                                            collNameSuffix,
                                            docs,
                                            [[indexSpecsFlat[0]], [indexSpecsFlat[1]]],
                                            rollbackStartFailPoints,
                                            3,  // rollbackStartFailPointsIteration
                                            rollbackEndFailPoints,
                                            1,  // rollbackEndFailPointsIteration
                                            locksYieldedFailPoints,
                                            resumePhases,
                                            resumeChecks,
                                            [{a: 11, b: 11}, {a: 12, b: 12}],
                                            sideWrites);
    };

    runTest([{a: 1, b: 1}, {a: 2, b: 2}, {a: 3, b: 3}, {a: 4, b: 4}, {a: 5, b: 5}],
            [{a: 6, b: 6}, {a: 7, b: 7}, {a: 8, b: 8}, {a: 9, b: 9}, {a: 10, b: 10}],
            [{a: 1}, {b: 1}],
            "");
    runTest([{a: [1, 2], b: 1}, {a: 2, b: 2}, {a: 3, b: 3}, {a: 4, b: 4}, {a: 5, b: 5}],
            [{a: 6, b: 6}, {a: 7, b: 7}, {a: 8, b: 8}, {a: 9, b: 9}, {a: 10, b: [10, 11]}],
            [{a: 1}, {b: 1}],
            "_multikey_1");
    runTest([{a: 1, b: 1}, {a: 2, b: 2}, {a: 3, b: 3}, {a: 4, b: 4}, {a: [5, 6], b: 5}],
            [{a: 6, b: [6, 7]}, {a: 7, b: 7}, {a: 8, b: 8}, {a: 9, b: 9}, {a: 10, b: 10}],
            [{a: 1}, {b: 1}],
            "_multikey_2");
    runTest([{a: [1, 2]}, {b: {c: [3, 4]}}, {d: ""}, {e: ""}, {f: [[]]}],
            [{g: null}, {h: 8}, {i: 9}, {j: [{}]}, {k: {}}],
            [{"$**": 1}, {h: 1}],
            "_wildcard");
};

runRollbackTo(
    [
        {name: "hangIndexBuildDuringCollectionScanPhaseBeforeInsertion", logIdWithBuildUUID: 20386},
        {name: "hangIndexBuildDuringBulkLoadPhase", logIdWithIndexName: 4924400}
    ],
    [
        {name: "hangAfterSettingUpIndexBuild", logIdWithBuildUUID: 20387},
        {name: "hangIndexBuildDuringCollectionScanPhaseAfterInsertion", logIdWithBuildUUID: 20386}
    ],
    ["setYieldAllLocksHang", "hangDuringIndexBuildBulkLoadYield"],
    ["collection scan", "bulk load"],
    [{numScannedAfterResume: 6}, {skippedPhaseLogID: 20391}]);

runRollbackTo(
    [
        {name: "hangIndexBuildDuringCollectionScanPhaseBeforeInsertion", logIdWithBuildUUID: 20386},
        {name: "hangIndexBuildDuringBulkLoadPhase", logIdWithIndexName: 4924400}
    ],
    [
        {name: "hangAfterSettingUpIndexBuild", logIdWithBuildUUID: 20387},
        {name: "hangIndexBuildDuringBulkLoadPhaseSecond", logIdWithIndexName: 4924400}
    ],
    ["setYieldAllLocksHang", "hangDuringIndexBuildBulkLoadYield"],
    ["collection scan", "bulk load"],
    [{numScannedAfterResume: 6}, {skippedPhaseLogID: 20391}]);

runRollbackTo(
    [
        {name: "hangIndexBuildDuringCollectionScanPhaseBeforeInsertion", logIdWithBuildUUID: 20386},
        {name: "hangIndexBuildDuringDrainWritesPhase", logIdWithIndexName: 4841800}
    ],
    [
        {name: "hangAfterSettingUpIndexBuild", logIdWithBuildUUID: 20387},
        {name: "hangIndexBuildDuringDrainWritesPhaseSecond", logIdWithIndexName: 4841800}
    ],
    ["setYieldAllLocksHang", "hangDuringIndexBuildDrainYield"],
    ["collection scan", "drain writes"],
    [{numScannedAfterResume: 6}, {skippedPhaseLogID: 20392}]);

runRollbackTo(
    [
        {name: "hangIndexBuildDuringCollectionScanPhaseBeforeInsertion", logIdWithBuildUUID: 20386},
        {name: "hangIndexBuildDuringBulkLoadPhase", logIdWithIndexName: 4924400}
    ],
    [
        {name: "hangIndexBuildDuringCollectionScanPhaseAfterInsertion", logIdWithBuildUUID: 20386},
        {name: "hangIndexBuildDuringBulkLoadPhaseSecond", logIdWithIndexName: 4924400}
    ],
    ["setYieldAllLocksHang", "hangDuringIndexBuildBulkLoadYield"],
    ["collection scan", "bulk load"],
    [{numScannedAfterResume: 6}, {skippedPhaseLogID: 20391}]);

runRollbackTo(
    [
        {name: "hangIndexBuildDuringCollectionScanPhaseBeforeInsertion", logIdWithBuildUUID: 20386},
        {name: "hangIndexBuildDuringDrainWritesPhase", logIdWithIndexName: 4841800}
    ],
    [
        {name: "hangIndexBuildDuringCollectionScanPhaseAfterInsertion", logIdWithBuildUUID: 20386},
        {name: "hangIndexBuildDuringDrainWritesPhaseSecond", logIdWithIndexName: 4841800}
    ],
    ["setYieldAllLocksHang", "hangDuringIndexBuildDrainYield"],
    ["collection scan", "drain writes"],
    [{numScannedAfterResume: 6}, {skippedPhaseLogID: 20392}]);

runRollbackTo(
    [
        {name: "hangIndexBuildDuringBulkLoadPhase", logIdWithIndexName: 4924400},
        {name: "hangIndexBuildDuringDrainWritesPhase", logIdWithIndexName: 4841800}
    ],
    [
        {name: "hangIndexBuildDuringBulkLoadPhaseSecond", logIdWithIndexName: 4924400},
        {name: "hangIndexBuildDuringDrainWritesPhaseSecond", logIdWithIndexName: 4841800}
    ],
    ["hangDuringIndexBuildBulkLoadYield", "hangDuringIndexBuildDrainYield"],
    ["bulk load", "drain writes"],
    [{skippedPhaseLogID: 20391}, {skippedPhaseLogID: 20392}]);

rollbackTest.stop();
})();
