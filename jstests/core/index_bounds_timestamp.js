// Index bounds generation tests for Timestamp values.
// This file tests whether index bounds for timestamps are generated properly in terms of
// inclusiveness and exactness.

(function() {
    "use strict";

    load("jstests/libs/analyze_plan.js");

    // Helper function to get the nCounted from the COUNT stage in an explain plan's executionStats.
    function getCountFromExecutionStats(executionStats) {
        let stages = getPlanStages(executionStats.executionStages, "COUNT");
        assert.eq(1, stages.length, "executionStats should have 1 COUNT stage");
        return stages[0].nCounted;
    }

    // Setup the test collection.
    let coll = db.index_bounds_timestamp;
    coll.drop();

    // Create an index on the ts and _id fields.
    assert.commandWorked(coll.createIndex({ts: 1, _id: 1}));

    // Insert some test documents.
    // NOTE: Inserting Timestamp() or Timestamp(0, 0) into a collection creates a Timestamp for the
    // current time. Max Timestamp value is Timestamp(2^32 - 1, 2^32 - 1).
    const documents = [
        {_id: 0, ts: new Timestamp(0, 1)},
        {_id: 1, ts: new Timestamp(0, Math.pow(2, 31))},
        {_id: 2, ts: new Timestamp(0, Math.pow(2, 32) - 1)},
        {_id: 3, ts: new Timestamp(1, 0)},
        {_id: 4, ts: new Timestamp(Math.pow(2, 32) - 1, Math.pow(2, 32) - 1)}
    ];
    assert.writeOK(coll.insert(documents));

    // Sanity check the timestamp bounds generation plan.
    let plan;

    // Check that count over (Timestamp(0, 0), Timestamp(2^32 - 1, 2^32 - 1)] is a covered query.
    plan = coll.explain("executionStats").find({ts: {$gt: Timestamp(0, 0)}}).count();
    assert(isIndexOnly(plan.queryPlanner.winningPlan), "ts $gt count should be a covered query");
    assert.eq(5, getCountFromExecutionStats(plan.executionStats), "ts $gt count should be 5");

    // Check that find over (Timestamp(0, 0), Timestamp(2^32 - 1, 2^32 - 1)] does not require a
    // FETCH stage when the query is covered by an index.
    plan =
        coll.explain("executionStats").find({ts: {$gt: Timestamp(0, 0)}}, {ts: 1, _id: 0}).finish();
    assert(isIndexOnly(plan.queryPlanner.winningPlan),
           "ts $gt find with project should be a covered query");

    // Check that count over [Timestamp(0, 0), Timestamp(2^32 - 1, 2^32 - 1)] is a covered query.
    plan = coll.explain("executionStats").find({ts: {$gte: Timestamp(0, 0)}}).count();
    assert(isIndexOnly(plan.queryPlanner.winningPlan), "ts $gte count should be a covered query");
    assert.eq(5, getCountFromExecutionStats(plan.executionStats), "ts $gte count should be 5");

    // Check that find over [Timestamp(0, 0), Timestamp(2^32 - 1, 2^32 - 1)] does not require a
    // FETCH stage when the query is covered by an index.
    plan = coll.explain("executionStats")
               .find({ts: {$gte: Timestamp(0, 0)}}, {ts: 1, _id: 0})
               .finish();
    assert(isIndexOnly(plan.queryPlanner.winningPlan),
           "ts $gte find with project should be a covered query");

    // Check that count over [Timestamp(0, 0), Timestamp(1, 0)) is a covered query.
    plan = coll.explain("executionStats").find({ts: {$lt: Timestamp(1, 0)}}).count();
    assert(isIndexOnly(plan.queryPlanner.winningPlan), "ts $lt count should be a covered query");
    assert.eq(3, getCountFromExecutionStats(plan.executionStats), "ts $lt count should be 3");

    // Check that find over [Timestamp(0, 0), Timestamp(1, 0)) does not require a FETCH stage when
    // the query is covered by an index.
    plan =
        coll.explain("executionStats").find({ts: {$lt: Timestamp(1, 0)}}, {ts: 1, _id: 0}).finish();
    assert(isIndexOnly(plan.queryPlanner.winningPlan),
           "ts $lt find with project should be a covered query");

    // Check that count over [Timestamp(0, 0), Timestamp(1, 0)] is a covered query.
    plan = coll.explain("executionStats").find({ts: {$lte: Timestamp(1, 0)}}).count();
    assert(isIndexOnly(plan.queryPlanner.winningPlan), "ts $lte count should be a covered query");
    assert.eq(4, getCountFromExecutionStats(plan.executionStats), "ts $lte count should be 4");

    // Check that find over [Timestamp(0, 0), Timestamp(1, 0)] does not require a FETCH stage when
    // the query is covered by an index.
    plan = coll.explain("executionStats")
               .find({ts: {$lte: Timestamp(1, 0)}}, {ts: 1, _id: 0})
               .finish();
    assert(isIndexOnly(plan.queryPlanner.winningPlan),
           "ts $lte find with project should be a covered query");

    // Check that count over (Timestamp(0, 1), Timestamp(1, 0)) is a covered query.
    plan = coll.explain("executionStats")
               .find({ts: {$gt: Timestamp(0, 1), $lt: Timestamp(1, 0)}})
               .count();
    assert(isIndexOnly(plan.queryPlanner.winningPlan),
           "ts $gt, $lt count should be a covered query");
    assert.eq(2, getCountFromExecutionStats(plan.executionStats), "ts $gt, $lt count should be 2");

    // Check that find over (Timestamp(0, 1), Timestamp(1, 0)) does not require a FETCH stage when
    // the query is covered by an index.
    plan = coll.explain("executionStats")
               .find({ts: {$gt: Timestamp(0, 1), $lt: Timestamp(1, 0)}}, {ts: 1, _id: 0})
               .finish();
    assert(isIndexOnly(plan.queryPlanner.winningPlan),
           "ts $gt, $lt find with project should be a covered query");

    // Check that count over (Timestamp(0, 1), Timestamp(1, 0)] is a covered query.
    plan = coll.explain("executionStats")
               .find({ts: {$gt: Timestamp(0, 1), $lte: Timestamp(1, 0)}})
               .count();
    assert(isIndexOnly(plan.queryPlanner.winningPlan),
           "ts $gt, $lte count should be a covered query");
    assert.eq(3, getCountFromExecutionStats(plan.executionStats), "ts $gt, $lte count should be 3");

    // Check that find over (Timestamp(0, 1), Timestamp(1, 0)] does not require a FETCH stage when
    // the query is covered by an index.
    plan = coll.explain("executionStats")
               .find({ts: {$gt: Timestamp(0, 1), $lte: Timestamp(1, 0)}}, {ts: 1, _id: 0})
               .finish();
    assert(isIndexOnly(plan.queryPlanner.winningPlan),
           "ts $gt, $lte find with project should be a covered query");

    // Check that count over [Timestamp(0, 1), Timestamp(1, 0)) is a covered query.
    plan = coll.explain("executionStats")
               .find({ts: {$gte: Timestamp(0, 1), $lt: Timestamp(1, 0)}})
               .count();
    assert(isIndexOnly(plan.queryPlanner.winningPlan),
           "ts $gte, $lt count should be a covered query");
    assert.eq(3, getCountFromExecutionStats(plan.executionStats), "ts $gte, $lt count should be 3");

    // Check that find over [Timestamp(0, 1), Timestamp(1, 0)) does not require a FETCH stage when
    // the query is covered by an index.
    plan = coll.explain("executionStats")
               .find({ts: {$gte: Timestamp(0, 1), $lt: Timestamp(1, 0)}}, {ts: 1, _id: 0})
               .finish();
    assert(isIndexOnly(plan.queryPlanner.winningPlan),
           "ts $gte, $lt find with project should be a covered query");

    // Check that count over [Timestamp(0, 1), Timestamp(1, 0)] is a covered query.
    plan = coll.explain("executionStats")
               .find({ts: {$gte: Timestamp(0, 1), $lte: Timestamp(1, 0)}})
               .count();
    assert(isIndexOnly(plan.queryPlanner.winningPlan),
           "ts $gte, $lte count should be a covered query");
    assert.eq(
        4, getCountFromExecutionStats(plan.executionStats), "ts $gte, $lte count should be 4");

    // Check that find over [Timestamp(0, 1), Timestamp(1, 0)] does not require a FETCH stage when
    // the query is covered by an index.
    plan = coll.explain("executionStats")
               .find({ts: {$gte: Timestamp(0, 1), $lte: Timestamp(1, 0)}}, {ts: 1, _id: 0})
               .finish();
    assert(isIndexOnly(plan.queryPlanner.winningPlan),
           "ts $gte, $lte find with project should be a covered query");
})();
