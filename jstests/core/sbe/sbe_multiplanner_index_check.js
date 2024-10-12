/**
 * Test dropping index during SBE multiplanning.
 * @tags: [
 *   # We are setting the failpoint only on primaries, if the query is run on a mongos, the test
 *   # will hang.
 *   assumes_against_mongod_not_mongos,
 *   # We are setting the failpoint only on primaries, so we need to disable reads from secondaries,
 *   # where the failpoint is not enabled.
 *   assumes_read_preference_unchanged,
 *   uses_parallel_shell,
 *   # Multi clients cannot share global fail points. When one client turns off a fail point, other
 *   # clients waiting on the fail point will get failed.
 *   multi_clients_incompatible,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/collection_drop_recreate.js");
load("jstests/libs/fail_point_util.js");
load("jstests/libs/parallel_shell_helpers.js");
load("jstests/libs/sbe_util.js");

if (checkSbeCompletelyDisabled(db)) {
    jsTestLog("Skipping test because SBE is disabled");
    quit();
}

const collName = jsTestName() + "_coll";
const coll = assertDropAndRecreateCollection(db, collName);
assert.commandWorked(coll.insert(Array.from({length: 1}, (_, i) => ({_id: i, a: i, b: i}))));
assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.createIndex({b: 1}));

// This fail point will pause the aggregate cmd after SBE multiplanning but before re-generating
// SBE plan due to pushing down $group into SBE.
const fp = configureFailPoint(db.getMongo(), "hangMultiPlannerBeforeAggPipelineRebuild");

const awaitShell = startParallelShell(funWithArgs(function(collName) {
                                          assert.commandFailedWithCode(db.runCommand({
                                              explain: {
                                                  aggregate: collName,
                                                  pipeline: [
                                                      {$match: {a: {$gte: 0}, b: {$gte: 0}}},
                                                      {$group: {_id: null, ids: {$push: "$_id"}}},
                                                  ],
                                                  cursor: {},
                                              },
                                              verbosity: "queryPlanner"
                                          }),
                                                                       ErrorCodes.QueryPlanKilled);
                                      }, collName), db.getMongo().port);

fp.wait();

coll.dropIndex({b: 1});

fp.off();
awaitShell();
})();
