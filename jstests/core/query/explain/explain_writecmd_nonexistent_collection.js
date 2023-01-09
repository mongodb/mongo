// Test explaining a delete command against a non-existent collection.
//
// @tags: [requires_non_retryable_writes, requires_fastcount,
// assumes_no_implicit_collection_creation_after_drop]
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");

function assertCollectionDoesNotExist(collName) {
    const collectionList = db.getCollectionInfos({name: collName});
    assert.eq(0, collectionList.length, collectionList);
}

const collName = "explain_delete_nonexistent_collection";
const coll = db[collName];
coll.drop();

// Explain of delete against a non-existent collection returns an EOF plan.
let explain = assert.commandWorked(
    db.runCommand({explain: {delete: collName, deletes: [{q: {a: 1}, limit: 0}]}}));
assert(planHasStage(db, explain.queryPlanner.winningPlan, "EOF"), explain);
assert(!planHasStage(db, explain.queryPlanner.winningPlan, "DELETE"), explain);

assertCollectionDoesNotExist(collName);

// Explain of an update with upsert:false returns an EOF plan.
explain = assert.commandWorked(db.runCommand(
    {explain: {update: collName, updates: [{q: {a: 1}, u: {$set: {b: 1}}, upsert: false}]}}));
assert(planHasStage(db, explain.queryPlanner.winningPlan, "EOF"), explain);
assert(!planHasStage(db, explain.queryPlanner.winningPlan, "UPDATE"), explain);
assertCollectionDoesNotExist(collName);

// Explain of an update with upsert:true returns an EOF plan, and does not create a collection.
explain = assert.commandWorked(db.runCommand(
    {explain: {update: collName, updates: [{q: {a: 1}, u: {$set: {b: 1}}, upsert: true}]}}));
assert(planHasStage(db, explain.queryPlanner.winningPlan, "EOF"), explain);
assert(!planHasStage(db, explain.queryPlanner.winningPlan, "UPDATE"), explain);
assertCollectionDoesNotExist(collName);
}());
