/**
 * Verifies that engine selection works as intended for `featureFlagGetExecutorDeferredEngineChoice`.
 * For example, that pushing down SBE-eligible pipeline stages to the QuerySolution occurs before
 * plan-based engine selection.
 *
 * @tags: [
 * requires_fcv_90
 * ]
 */

import {getEngine} from "jstests/libs/query/analyze_plan.js";
import {checkSbeCompletelyDisabled, checkSbeFullyEnabled} from "jstests/libs/query/sbe_util.js";

const conn = MongoRunner.runMongod({setParameter: {featureFlagGetExecutorDeferredEngineChoice: true}});
const db = conn.getDB("engine_selection");

// This test expects SBE to be selected, which cannot happen when forceClassicEngine or SBE full
// are set.
if (checkSbeCompletelyDisabled(db) || checkSbeFullyEnabled(db)) {
    jsTest.log(
        "Exiting early because forceClassicEngine is set or SBE is fully enabled, " +
            "so engine selection won't be used.",
    );
    MongoRunner.stopMongod(conn);
    quit();
}

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
