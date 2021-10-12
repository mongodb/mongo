// Tests that the internal 'sbe' command is disallowed inside a transaction.
//
// @tags: [
//   assumes_against_mongod_not_mongos,
//   does_not_support_stepdowns,
//   uses_testing_only_commands,
//   uses_transactions,
// ]

(function() {
"use strict";

load("jstests/libs/sbe_util.js");  // For checkSBEEnabled.

const isSBEEnabled = checkSBEEnabled(db);
if (!isSBEEnabled) {
    jsTestLog("Skipping test because the SBE feature flag is disabled");
    return;
}

const dbName = "sbe_cmd_disallowed_in_txn_db";
const collName = "sbe_cmd_disallowed_in_txn_coll";
const testDb = db.getSiblingDB(dbName);
testDb.dropDatabase();

const coll = testDb[collName];
assert.commandWorked(coll.insertMany([
    {_id: 0, a: 1, b: 1, c: 1},
    {_id: 1, a: 1, b: 1, c: 2},
    {_id: 2, a: 1, b: 1, c: 3},
    {_id: 3, a: 1, b: 2, c: 3}
]));

// Use explain to obtain a test SBE command string.
const explain = coll.find({a: 1, b: 2}).explain();
if (!explain.queryPlanner.winningPlan.hasOwnProperty("slotBasedPlan")) {
    jsTestLog("Skipping test because the SBE feature flag is disabled");
    return;
}
const slotBasedPlan = explain.queryPlanner.winningPlan.slotBasedPlan;
assert(slotBasedPlan.hasOwnProperty("stages"), explain);
const sbeString = slotBasedPlan.stages;

const session = testDb.getMongo().startSession();
const sessionDb = session.getDatabase(dbName);

session.startTransaction();
assert.throwsWithCode(() => sessionDb._sbe(slotBasedPlan).itcount(),
                      ErrorCodes.OperationNotSupportedInTransaction);
session.abortTransaction();
}());
