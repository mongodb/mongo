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
const coll = rollbackTest.getPrimary().getDB(dbName).getCollection(jsTestName());

assert.commandWorked(coll.insert([{a: 1}, {a: 2}, {a: 3}, {a: 4}, {a: 5}]));

const runTest = function(rollbackEndFailPointName, rollbackEndFailPointIteration) {
    RollbackResumableIndexBuildTest.run(rollbackTest,
                                        dbName,
                                        coll.getName(),
                                        {a: 1},
                                        "hangIndexBuildDuringCollectionScanPhaseBeforeInsertion",
                                        3,  // rollbackStartFailPointIteration
                                        rollbackEndFailPointName,
                                        rollbackEndFailPointIteration,
                                        "setYieldAllLocksHang",
                                        "collection scan",
                                        {numScannedAferResume: 1},
                                        [{a: 6}, {a: 7}]);
};

// Rollback to before the index begins to be built.
runTest("hangAfterSettingUpIndexBuild", {});

// Rollback to earlier in the collection scan phase.
runTest("hangIndexBuildDuringCollectionScanPhaseAfterInsertion", 1);

rollbackTest.stop();
})();
