// This test checks that .explain() reports the correct trial period statistics for a winning plan
// in the "allPlansExecution" section.
(function() {
"use strict";

const dbName = "test";
const collName = jsTestName();

const conn = MongoRunner.runMongod({});
assert.neq(conn, null, "mongod failed to start up");

const db = conn.getDB(dbName);
const coll = db[collName];
coll.drop();

assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.createIndex({b: 1}));

// Configure the server such that the trial period should end after doing 10 reads from storage.
assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryPlanEvaluationWorks: 10}));
assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryPlanEvaluationWorksSbe: 10}));

assert.commandWorked(coll.insert(Array.from({length: 20}, (v, i) => {
    return {a: 1, b: 1, c: i};
})));

const explain = coll.find({a: 1, b: 1}).sort({c: 1}).explain("allPlansExecution");

// Since there are 20 documents, we expect that the full execution plan reports at least 20 keys
// examined and 20 docs examined.
//
// It is possible that more than 20 keys/docs are examined, because we expect the plan to be closed
// and re-opened during the trial period when running with SBE (which does not clear the execution
// stats from the first open).
assert.gte(explain.executionStats.totalKeysExamined, 20);
assert.gte(explain.executionStats.totalDocsExamined, 20);

// Extract the first plan in the "allPlansExecution" array. The winning plan is always reported
// first.
const winningPlanTrialPeriodStats = explain.executionStats.allPlansExecution[0];

// The number of keys examined and docs examined should both be less than 10, since we configured
// the trial period for each candidate plan to end after 10 storage reads.
assert.lte(winningPlanTrialPeriodStats.totalKeysExamined, 10);
assert.lte(winningPlanTrialPeriodStats.totalDocsExamined, 10);

MongoRunner.stopMongod(conn);
}());
