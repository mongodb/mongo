/**
 * Tests that resumable index builds restart and complete properly when rolling back from after the
 * index build completed to while the index build was still in progress.
 *
 * TODO (SERVER-49075): Move this test to the replica_sets suite once it is enabled on the resumable
 * index builds variant.
 *
 * @tags: [
 *   requires_persistence,
 *   requires_replication,
 * ]
 */
(function() {
"use strict";

load('jstests/replsets/libs/rollback_resumable_index_build.js');

const dbName = "test";
const insertsToBeRolledBack = [{a: 7}, {a: 8}];

const rollbackTest = new RollbackTest(jsTestName());
const coll = rollbackTest.getPrimary().getDB(dbName).getCollection(jsTestName());

assert.commandWorked(coll.insert([{a: 1}, {a: 2}, {a: 3}]));

// Rollback to before the index begins to be built.
RollbackResumableIndexBuildTest.runIndexBuildComplete(rollbackTest,
                                                      dbName,
                                                      coll.getName(),
                                                      {a: 1},
                                                      "hangAfterSettingUpIndexBuildUnlocked",
                                                      {},
                                                      insertsToBeRolledBack);

// Rollback to the collection scan phase.
RollbackResumableIndexBuildTest.runIndexBuildComplete(
    rollbackTest,
    dbName,
    coll.getName(),
    {a: 1},
    "hangIndexBuildDuringCollectionScanPhaseBeforeInsertion",
    {fieldsToMatch: {a: 2}},
    insertsToBeRolledBack);

// Rollback to the bulk load phase.
RollbackResumableIndexBuildTest.runIndexBuildComplete(rollbackTest,
                                                      dbName,
                                                      coll.getName(),
                                                      {a: 1},
                                                      "hangIndexBuildDuringBulkLoadPhase",
                                                      {iteration: 1},
                                                      insertsToBeRolledBack);

// Rollback to the drain writes phase.
RollbackResumableIndexBuildTest.runIndexBuildComplete(rollbackTest,
                                                      dbName,
                                                      coll.getName(),
                                                      {a: 1},
                                                      "hangIndexBuildDuringDrainWritesPhase",
                                                      {iteration: 1},
                                                      insertsToBeRolledBack,
                                                      [{a: 4}, {a: 5}, {a: 6}]);

rollbackTest.stop();
})();