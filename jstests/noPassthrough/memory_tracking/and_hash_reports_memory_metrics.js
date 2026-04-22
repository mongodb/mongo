/**
 * Tests that, when the memory tracking feature flag is enabled, memory tracking statistics are
 * reported to the slow query log, system.profile, and explain("executionStats") for queries that
 * use the SBE AndHash stage (triggered by AND_HASH index intersection plans).
 *
 * AND_HASH is used when index scans are not sorted by RecordId (i.e. range predicates). It eagerly
 * builds a hash table of all outer-side records during open(), so its memory is non-zero for the
 * entire query lifetime and drops to zero only when the cursor is closed.
 *
 * @tags: [
 *   requires_profiling,
 *   requires_getmore,
 *   # The test queries the system.profile collection so it is not compatible with initial sync
 *   # since an initial sync may insert unexpected operations into the profile collection.
 *   queries_system_profile_collection,
 *   # The test runs the profile and getLog commands, which are not supported in Serverless.
 *   command_not_supported_in_serverless,
 *   assumes_against_mongod_not_mongos,
 *   requires_fcv_90,
 * ]
 */

import {getWinningPlanFromExplain, planHasStage} from "jstests/libs/query/analyze_plan.js";
import {runMemoryStatsTest} from "jstests/libs/query/memory_tracking_utils.js";

const conn = MongoRunner.runMongod();
assert.neq(null, conn, "mongod was unable to start up");
const db = conn.getDB("test");

// Force SBE to be fully enabled so the SBE AndHash stage is used.
assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "trySbeEngine"}));

// Force AND_HASH index intersection plans so the SBE AndHash stage is used.
// internalQueryPlannerEnableHashIntersection enables AND_HASH plan generation;
// internalQueryForceIntersectionPlans boosts intersection plan scores in the ranker.
assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryPlannerEnableHashIntersection: true}));
assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryForceIntersectionPlans: true}));

const coll = db[jsTestName()];
coll.drop();

const docs = [];
for (let i = 0; i < 20; i++) {
    docs.push({_id: i, a: i, b: i});
}
assert.commandWorked(coll.insertMany(docs));
assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.createIndex({b: 1}));

// Range predicates on two indexed fields with forced hash intersection produce an AND_HASH plan.
const pipeline = [{$match: {a: {$gte: 0}, b: {$gte: 0}}}];

// Verify that the planner actually chose an AND_HASH plan before running the memory test.
const explainRes = coll.explain("queryPlanner").aggregate(pipeline);
assert(
    planHasStage(db, getWinningPlanFromExplain(explainRes), "AND_HASH"),
    "Expected AND_HASH stage in winning plan but got: " + tojson(explainRes),
);

// Skip if AND_HASH did not execute in SBE. When featureFlagGetExecutorDeferredEngineChoice is
// enabled, isPlanSbeEligible() rejects AND_HASH plans via AndHashOrSortedRule (SERVER-90818),
// causing them to fall back to the classic engine even with trySbeEngine set.
const execExplain = coll.explain("executionStats").aggregate(pipeline, {allowDiskUse: false});
if (execExplain.explainVersion !== "2") {
    jsTest.log.info("Skipping memory tracking assertions: AND_HASH ran in classic engine, not SBE AndHash.");
    coll.drop();
    MongoRunner.stopMongod(conn);
    quit();
}

runMemoryStatsTest({
    db,
    collName: coll.getName(),
    commandObj: {
        aggregate: coll.getName(),
        pipeline,
        comment: "memory stats and_hash test",
        allowDiskUse: false,
        cursor: {batchSize: 5},
    },
    stageName: "and_hash",
    expectedNumGetMores: 3,
});

// Clean up.
coll.drop();
MongoRunner.stopMongod(conn);
