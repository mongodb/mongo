/**
 * Tests the pipeline-style update is accepted by the findAndModify command.
 * @tags: [requires_non_retryable_writes]
 */
(function() {
"use strict";

load("jstests/libs/fixture_helpers.js");  // For isMongos.
load("jstests/libs/analyze_plan.js");     // For planHasStage().

const coll = db.find_and_modify_pipeline_update;
coll.drop();

// Test that it generally works.
assert.commandWorked(coll.insert([{_id: 0}, {_id: 1}, {_id: 2}, {_id: 3}, {_id: 4}]));
let found = coll.findAndModify({query: {_id: 0}, update: [{$set: {y: 1}}]});
assert.eq(found, {_id: 0});
found = coll.findAndModify({query: {_id: 0}, update: [{$set: {z: 2}}], new: true});
assert.eq(found, {_id: 0, y: 1, z: 2});

found = coll.findAndModify({query: {_id: 0}, update: [{$unset: ["z"]}], new: true});
assert.eq(found, {_id: 0, y: 1});

// Test that pipeline-style update supports the 'fields' argument.
assert(coll.drop());
assert.commandWorked(coll.insert([{_id: 0, x: 0}, {_id: 1, x: 1}, {_id: 2, x: 2}, {_id: 3, x: 3}]));
found = coll.findAndModify({query: {_id: 0}, update: [{$set: {y: 0}}], fields: {x: 0}});
assert.eq(found, {_id: 0});

found = coll.findAndModify({query: {_id: 1}, update: [{$set: {y: 1}}], fields: {x: 1}});
assert.eq(found, {_id: 1, x: 1});

found = coll.findAndModify({query: {_id: 2}, update: [{$set: {y: 2}}], fields: {x: 0}, new: true});
assert.eq(found, {_id: 2, y: 2});

found = coll.findAndModify({query: {_id: 3}, update: [{$set: {y: 3}}], fields: {x: 1}, new: true});
assert.eq(found, {_id: 3, x: 3});

//
// Tests for explain using findAndModify with an _id equality query.
//
{
    let explain =
        coll.explain("queryPlanner").findAndModify({query: {_id: 3}, update: [{$set: {y: 999}}]});
    assert(planHasStage(db, explain.queryPlanner.winningPlan, "IDHACK"));
    assert(planHasStage(db, explain.queryPlanner.winningPlan, "UPDATE"));

    // Run explain with execution-level verbosity.
    explain =
        coll.explain("executionStats").findAndModify({query: {_id: 3}, update: [{$set: {y: 999}}]});
    assert.eq(explain.executionStats.nReturned, 1);
    // UPDATE stage would modify one document.
    const updateStage = getPlanStage(explain.executionStats.executionStages, "UPDATE");
    assert.eq(updateStage.nWouldModify, 1);

    // Check that no write was performed.
    assert.eq(coll.find({y: 999}).count(), 0);
}

// We skip the following tests for sharded fixtures as it will fail as the query when
// findAndModify doesn't contain the shard key.
if (!FixtureHelpers.isMongos(db)) {
    //
    // Tests for explain with a query that requires a COLLSCAN.
    //
    let explain =
        coll.explain("queryPlanner").findAndModify({query: {y: 3}, update: [{$set: {y: 999}}]});
    assert(planHasStage(db, explain.queryPlanner.winningPlan, "COLLSCAN"));
    assert(planHasStage(db, explain.queryPlanner.winningPlan, "UPDATE"));

    // Run explain with execution-level verbosity.
    explain =
        coll.explain("executionStats").findAndModify({query: {y: 3}, update: [{$set: {y: 999}}]});
    assert.eq(explain.executionStats.nReturned, 1);
    // UPDATE stage would modify one document.
    const updateStage = getPlanStage(explain.executionStats.executionStages, "UPDATE");
    assert.eq(updateStage.nWouldModify, 1);

    // Check that no write was performed.
    assert.eq(coll.find({y: 999}).count(), 0);

    //
    // Test that 'sort' works with pipeline-style update.
    //
    assert(coll.drop());
    assert.commandWorked(
        coll.insert([{_id: 0, x: 'b'}, {_id: 1, x: 'd'}, {_id: 2, x: 'a'}, {_id: 3, x: 'c'}]));
    found = coll.findAndModify({update: [{$set: {foo: "bar"}}], sort: {x: -1}, new: true});
    assert.eq(found, {_id: 1, x: 'd', foo: "bar"});
}

// Test that it rejects the combination of arrayFilters and a pipeline-style update.
let err =
    assert.throws(() => coll.findAndModify(
                      {query: {_id: 1}, update: [{$set: {y: 1}}], arrayFilters: [{"i.x": 4}]}));
assert.eq(err.code, ErrorCodes.FailedToParse);
}());
