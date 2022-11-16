// When running explain commands with "executionStats" verbosity, checks that the explain output
// includes "executionTimeMicros"/"executionTimeNanos" only if requested.
// "executionTimeMillisEstimate" will always be present in the explain output.
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");  // For getAllPlanStages().

let conn = MongoRunner.runMongod({});
assert.neq(conn, null, "mongod failed to start up");

let db = conn.getDB("test");
let coll = db.explain_execution_time_in_nanoseconds;
coll.drop();

assert.commandWorked(coll.createIndex({x: 1}));
assert.commandWorked(coll.createIndex({y: 1}));

for (let i = 0; i < 1000; i++) {
    assert.commandWorked(coll.insert({x: i, y: i * 2}));
}

function verifyStages(execStages, microAndNanosExpected) {
    const allStages = getAllPlanStages(execStages);
    assert.eq(execStages.hasOwnProperty("executionTimeMicros"), microAndNanosExpected);
    assert.eq(execStages.hasOwnProperty("executionTimeNanos"), microAndNanosExpected);
    for (let stage of allStages) {
        assert(stage.hasOwnProperty("executionTimeMillisEstimate"), stage);
        assert.eq(stage.hasOwnProperty("executionTimeMicros"), microAndNanosExpected, stage);
        assert.eq(stage.hasOwnProperty("executionTimeNanos"), microAndNanosExpected, stage);
    }
}

// Test explain on find command.
let explainResult = coll.find({x: {$gt: 500}}).explain("executionStats");
let executionStages = explainResult.executionStats.executionStages;
assert(executionStages.hasOwnProperty("executionTimeMillisEstimate"), executionStages);
verifyStages(executionStages, false);

// Test explain on aggregate command.
const pipeline = [{$match: {x: {$gt: 500}}}, {$addFields: {xx: {$add: ["$x", "$y"]}}}];
// Run an explain command when the "executionTimeMicros"/"executionTimeNanos" should be omitted.
explainResult = coll.explain("executionStats").aggregate(pipeline);
executionStages = explainResult.stages;
assert.neq(executionStages.length, 0, executionStages);

for (let executionStage of executionStages) {
    // We should only have "executionTimeMillisEstimate" in the explain output.
    assert(executionStage.hasOwnProperty("executionTimeMillisEstimate"), executionStage);
    assert(!executionStage.hasOwnProperty("executionTimeMicros"), executionStage);
    assert(!executionStage.hasOwnProperty("executionTimeNanos"), executionStage);
    if (executionStage.hasOwnProperty("$cursor")) {
        const stages = executionStage["$cursor"]["executionStats"]["executionStages"];
        verifyStages(stages, false);
    }
}

MongoRunner.stopMongod(conn);

// Request microsecond precision for the estimates of execution time.
conn = MongoRunner.runMongod({setParameter: "internalMeasureQueryExecutionTimeInNanoseconds=true"});
assert.neq(conn, null, "mongod failed to start up");
db = conn.getDB("test");
coll = db.explain_execution_time_in_microseconds;

explainResult = coll.find({x: {$gt: 500}}).explain("executionStats");
let isSBE = explainResult.explainVersion === "2";
executionStages = explainResult.executionStats.executionStages;
assert(executionStages.hasOwnProperty("executionTimeMillisEstimate"), executionStages);
verifyStages(executionStages, isSBE);

explainResult = coll.explain("executionStats").aggregate(pipeline);
executionStages = explainResult.stages;
isSBE = explainResult.explainVersion === "2";
assert.neq(executionStages.length, 0, executionStages);
for (let executionStage of executionStages) {
    assert(executionStage.hasOwnProperty("executionTimeMillisEstimate"), executionStage);
    // "executionTimeMicros"/"executionTimeNanos" is only added to SBE stages, not to agg stages.
    assert(!executionStage.hasOwnProperty("executionTimeMicros"), executionStage);
    assert(!executionStage.hasOwnProperty("executionTimeNanos"), executionStage);

    if (executionStage.hasOwnProperty("$cursor")) {
        const stages = executionStage["$cursor"]["executionStats"]["executionStages"];
        verifyStages(stages, isSBE);
    }
}

MongoRunner.stopMongod(conn);
})();
