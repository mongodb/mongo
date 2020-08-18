/**
 * Tests that resumable index builds complete properly after being interrupted for rollback during
 * the bulk load phase.
 *
 * @tags: [
 *   requires_majority_read_concern,
 *   requires_persistence,
 * ]
 */
(function() {
"use strict";

load('jstests/replsets/libs/rollback_resumable_index_build.js');

const dbName = "test";
const rollbackStartFailPointName = "hangIndexBuildDuringBulkLoadPhase";
const insertsToBeRolledBack = [{a: 4}, {a: 5}];

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
                                    "hangAfterSettingUpIndexBuildUnlocked",
                                    {},
                                    insertsToBeRolledBack);

// Rollback to the collection scan phase.
RollbackResumableIndexBuildTest.run(rollbackTest,
                                    dbName,
                                    coll.getName(),
                                    {a: 1},
                                    rollbackStartFailPointName,
                                    {iteration: 1},
                                    "hangIndexBuildDuringCollectionScanPhaseBeforeInsertion",
                                    {fieldsToMatch: {a: 2}},
                                    insertsToBeRolledBack);

rollbackTest.stop();
})();