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
const rollbackStartFailPointName = "hangIndexBuildDuringCollectionScanPhaseBeforeInsertion";
const insertsToBeRolledBack = [{a: 6}, {a: 7}];

const rollbackTest = new RollbackTest(jsTestName());
const coll = rollbackTest.getPrimary().getDB(dbName).getCollection(jsTestName());

assert.commandWorked(coll.insert([{a: 1}, {a: 2}, {a: 3}, {a: 4}, {a: 5}]));

// Rollback to before the index begins to be built.
RollbackResumableIndexBuildTest.run(rollbackTest,
                                    dbName,
                                    coll.getName(),
                                    {a: 1},
                                    rollbackStartFailPointName,
                                    {iteration: 3},
                                    "hangAfterSettingUpIndexBuild",
                                    {},
                                    "setYieldAllLocksHang",
                                    insertsToBeRolledBack);

// Rollback to earlier in the collection scan phase.
RollbackResumableIndexBuildTest.run(rollbackTest,
                                    dbName,
                                    coll.getName(),
                                    {a: 1},
                                    rollbackStartFailPointName,
                                    {iteration: 3},
                                    "hangIndexBuildDuringCollectionScanPhaseAfterInsertion",
                                    {iteration: 1},
                                    "setYieldAllLocksHang",
                                    insertsToBeRolledBack);

rollbackTest.stop();
})();
