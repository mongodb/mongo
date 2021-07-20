/**
 * Tests that a resumable index build large enough to spill to disk during the collection scan
 * phase completes properly after being interrupted for rollback during the bulk load phase.
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

const numDocuments = 100;
const maxIndexBuildMemoryUsageMB = 50;

const rollbackTest = new RollbackTest(jsTestName());

// Insert enough data so that the collection scan spills to disk.
const docs = [];
for (let i = 0; i < numDocuments; i++) {
    // Each document is at least 1 MB.
    docs.push({a: i.toString().repeat(1024 * 1024)});
}

const runRollbackTo = function(rollbackEndFailPoint) {
    assert.commandWorked(rollbackTest.getPrimary().adminCommand(
        {setParameter: 1, maxIndexBuildMemoryUsageMegabytes: maxIndexBuildMemoryUsageMB}));

    RollbackResumableIndexBuildTest.run(
        rollbackTest,
        dbName,
        "_large",
        docs,
        [[{a: 1}]],
        [{name: "hangIndexBuildDuringBulkLoadPhase", logIdWithIndexName: 4924400}],
        // Each document is at least 1 MB, so the index build must have spilled to disk by this
        // point.
        maxIndexBuildMemoryUsageMB,  // rollbackStartFailPointsIteration
        [rollbackEndFailPoint],
        1,  // rollbackEndFailPointsIteration
        ["hangDuringIndexBuildBulkLoadYield"],
        ["bulk load"],
        [{skippedPhaseLogID: 20391}],
        [{a: 1}, {a: 2}],
        [],
        {skipDataConsistencyChecks: true});
};

// Rollback to before the indexes begin to be built.
runRollbackTo({name: "hangAfterSettingUpIndexBuild", logIdWithBuildUUID: 20387});

// Rollback to earlier in the collection scan phase.
runRollbackTo(
    {name: "hangIndexBuildDuringCollectionScanPhaseBeforeInsertion", logIdWithBuildUUID: 20386});

// Rollback to the bulk load phase.
runRollbackTo({name: "hangIndexBuildDuringBulkLoadPhaseSecond", logIdWithIndexName: 4924400});

rollbackTest.stop();
})();
