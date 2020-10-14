/**
 * Tests that a resumable index build large enough to spill to disk during the collection scan
 * phase completes properly after being interrupted for rollback during the collection scan phase.
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
const coll = rollbackTest.getPrimary().getDB(dbName).getCollection(jsTestName());

// Insert enough data so that the collection scan spills to disk.
const bulk = coll.initializeUnorderedBulkOp();
for (let i = 0; i < numDocuments; i++) {
    // Each document is at least 1 MB.
    bulk.insert({a: i.toString().repeat(1024 * 1024)});
}
assert.commandWorked(bulk.execute());

const runTest = function(rollbackEndFailPointName, rollbackEndFailPointIteration) {
    assert.commandWorked(rollbackTest.getPrimary().adminCommand(
        {setParameter: 1, maxIndexBuildMemoryUsageMegabytes: maxIndexBuildMemoryUsageMB}));

    RollbackResumableIndexBuildTest.run(
        rollbackTest,
        dbName,
        coll.getName(),
        {a: 1},
        "hangIndexBuildDuringCollectionScanPhaseBeforeInsertion",
        // Each document is at least 1 MB, so the index build must have spilled to disk by this
        // point.
        maxIndexBuildMemoryUsageMB,  // rollbackStartFailPointIteration
        rollbackEndFailPointName,
        rollbackEndFailPointIteration,
        "setYieldAllLocksHang",
        "collection scan",
        // The collection scan will scan one additional document past the point specified above due
        // to locks needing to be yielded before the rollback can occur. Thus, we subtract 1 from
        // the difference.
        {numScannedAferResume: numDocuments - maxIndexBuildMemoryUsageMB - 1},
        [{a: 1}, {a: 2}]);
};

// Rollback to before the index begins to be built.
runTest("hangAfterSettingUpIndexBuild", {});

// Rollback to earlier in the collection scan phase.
runTest("hangIndexBuildDuringCollectionScanPhaseAfterInsertion", 1);

rollbackTest.stop();
})();
