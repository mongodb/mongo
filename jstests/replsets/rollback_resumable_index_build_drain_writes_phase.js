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
const rollbackStartFailPointName = "hangIndexBuildDuringDrainWritesPhase";
const insertsToBeRolledBack = [{a: 18}, {a: 19}];

const rollbackTest = new RollbackTest(jsTestName());
const coll = rollbackTest.getPrimary().getDB(dbName).getCollection(jsTestName());

assert.commandWorked(coll.insert([{a: 1}, {a: 2}, {a: 3}]));

// Rollback to before the index begins to be built.
RollbackResumableIndexBuildTest.run(rollbackTest,
                                    dbName,
                                    coll.getName(),
                                    {a: 1},
                                    rollbackStartFailPointName,
                                    {iteration: 1},
                                    "hangAfterSettingUpIndexBuild",
                                    {},
                                    "hangDuringIndexBuildDrainYield",
                                    insertsToBeRolledBack,
                                    [{a: 4}, {a: 5}, {a: 6}]);

// Rollback to the collection scan phase.
RollbackResumableIndexBuildTest.run(rollbackTest,
                                    dbName,
                                    coll.getName(),
                                    {a: 1},
                                    rollbackStartFailPointName,
                                    {iteration: 1},
                                    "hangIndexBuildDuringCollectionScanPhaseBeforeInsertion",
                                    {iteration: 1},
                                    "hangDuringIndexBuildDrainYield",
                                    insertsToBeRolledBack,
                                    [{a: 7}, {a: 8}, {a: 9}]);

// Rollback to the bulk load phase.
RollbackResumableIndexBuildTest.run(rollbackTest,
                                    dbName,
                                    coll.getName(),
                                    {a: 1},
                                    rollbackStartFailPointName,
                                    {iteration: 1},
                                    "hangIndexBuildDuringBulkLoadPhase",
                                    {iteration: 1},
                                    "hangDuringIndexBuildDrainYield",
                                    insertsToBeRolledBack,
                                    [{a: 10}, {a: 11}, {a: 12}]);

// Rollback to earlier in the drain writes phase.
RollbackResumableIndexBuildTest.run(rollbackTest,
                                    dbName,
                                    coll.getName(),
                                    {a: 1},
                                    rollbackStartFailPointName,
                                    {iteration: 3},
                                    "hangIndexBuildDuringDrainWritesPhaseSecond",
                                    {iteration: 1},
                                    "hangDuringIndexBuildDrainYield",
                                    insertsToBeRolledBack,
                                    [{a: 13}, {a: 14}, {a: 15}, {a: 16}, {a: 17}]);

rollbackTest.stop();
})();
