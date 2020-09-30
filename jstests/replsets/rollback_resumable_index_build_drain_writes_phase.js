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
const coll = rollbackTest.getPrimary().getDB(dbName).getCollection(jsTestName());

assert.commandWorked(coll.insert([{a: 1}, {a: 2}, {a: 3}]));

const runTest = function(rollbackStartFailPointIteration,
                         rollbackEndFailPointName,
                         rollbackEndFailPointIteration,
                         sideWrites) {
    RollbackResumableIndexBuildTest.run(rollbackTest,
                                        dbName,
                                        coll.getName(),
                                        {a: 1},
                                        "hangIndexBuildDuringDrainWritesPhase",
                                        rollbackStartFailPointIteration,
                                        rollbackEndFailPointName,
                                        rollbackEndFailPointIteration,
                                        "hangDuringIndexBuildDrainYield",
                                        "drain writes",
                                        {skippedPhaseLogID: 20392},
                                        [{a: 18}, {a: 19}],
                                        sideWrites);
};

// Rollback to before the index begins to be built.
runTest(1, "hangAfterSettingUpIndexBuild", {}, [{a: 4}, {a: 5}, {a: 6}]);

// Rollback to the collection scan phase.
runTest(1, "hangIndexBuildDuringCollectionScanPhaseBeforeInsertion", 1, [{a: 7}, {a: 8}, {a: 9}]);

// Rollback to the bulk load phase.
runTest(1, "hangIndexBuildDuringBulkLoadPhase", 1, [{a: 10}, {a: 11}, {a: 12}]);

// Rollback to earlier in the drain writes phase.
runTest(3,
        "hangIndexBuildDuringDrainWritesPhaseSecond",
        1,
        [{a: 13}, {a: 14}, {a: 15}, {a: 16}, {a: 17}]);

rollbackTest.stop();
})();
