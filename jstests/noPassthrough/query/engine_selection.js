/**
 * Verifies that engine selection works as intended for `featureFlagGetExecutorDeferredEngineChoice`.
 * For example, that pushing down SBE-eligible pipeline stages to the QuerySolution occurs before
 * plan-based engine selection.
 *
 * @tags: [
 * featureFlagSbeEqLookupUnwind
 * ]
 */

import {getEngine} from "jstests/libs/query/analyze_plan.js";

const conn = MongoRunner.runMongod({setParameter: {featureFlagGetExecutorDeferredEngineChoice: true}});
const db = conn.getDB("engine_selection");

// Set logLevel to 1 so that all queries will be logged.
assert.commandWorked(db.setLogLevel(1));

const localColl = db.localColl;
assert(localColl.drop());
assert.commandWorked(localColl.insert({a: 1}));

const foreignColl = db.foreignColl;
assert(foreignColl.drop());
assert.commandWorked(foreignColl.insert({a: 1}));

// Run a $lookup-$unwind, then assert that the plan-based engine selection logging shows that the LU node was on the QuerySolution.
const explain = localColl
    .explain()
    .aggregate([{$lookup: {from: "foreignColl", as: "res", localField: "a", foreignField: "a"}}, {$unwind: "$res"}]);
assert.eq(getEngine(explain), "sbe");

const logs = assert.commandWorked(db.adminCommand({getLog: "global"})).log;
const planSelectionLogs = logs.filter((log) => log.includes("11986305"));
assert.eq(planSelectionLogs.length, 1);
assert(planSelectionLogs[0].includes("EQ_LOOKUP_UNWIND"));

MongoRunner.stopMongod(conn);
