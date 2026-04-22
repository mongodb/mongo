/**
 * Tests that, when the memory tracking feature flag is enabled, memory tracking statistics are
 * reported to the slow query log, system.profile, and explain("executionStats") for queries that
 * use the SBE MergeJoin stage (triggered by AND_SORTED index intersection plans).
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

// Force SBE to be fully enabled so the SBE MergeJoin stage is used.
assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "trySbeEngine"}));

// Force AND_SORTED index intersection plans so the SBE MergeJoin stage is used.
// internalQueryPlannerEnableSortIndexIntersection enables AND_SORTED plan generation;
// internalQueryForceIntersectionPlans boosts intersection plan scores in the ranker.
assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryPlannerEnableSortIndexIntersection: true}));
assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryForceIntersectionPlans: true}));

const coll = db[jsTestName()];
coll.drop();

// All documents share the same 'a' and 'b' values so that equality predicates match all docs.
// Equality predicates make sortedByDiskLoc() == true on both index scans, which is required for
// the planner to select AND_SORTED over AND_HASH.
const docs = [];
for (let i = 0; i < 20; i++) {
    docs.push({_id: i, a: 0, b: 0});
}
assert.commandWorked(coll.insertMany(docs));
assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.createIndex({b: 1}));

// A $match with equality predicates on two indexed fields produces an AND_SORTED plan when index
// intersection is forced. The SBE stage builder translates AND_SORTED into a MergeJoin stage.
const pipeline = [{$match: {a: 0, b: 0}}];

// Verify that the planner actually chose an AND_SORTED plan before running the memory test.
const explainRes = coll.explain("queryPlanner").aggregate(pipeline);
assert(
    planHasStage(db, getWinningPlanFromExplain(explainRes), "AND_SORTED"),
    "Expected AND_SORTED stage in winning plan but got: " + tojson(explainRes),
);

// Skip if AND_SORTED did not execute in SBE. When featureFlagGetExecutorDeferredEngineChoice is
// enabled, isPlanSbeEligible() rejects AND_SORTED plans via AndHashOrSortedRule (SERVER-90818),
// causing them to fall back to the classic engine even with trySbeEngine set.
const execExplain = coll.explain("executionStats").aggregate(pipeline, {allowDiskUse: false});
if (execExplain.explainVersion !== "2") {
    jsTest.log.info("Skipping memory tracking assertions: AND_SORTED ran in classic engine, not SBE MergeJoin.");
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
        comment: "memory stats merge join test",
        allowDiskUse: false,
        cursor: {batchSize: 5},
    },
    stageName: "mj",
    expectedNumGetMores: 3,
});

// Clean up.
coll.drop();
MongoRunner.stopMongod(conn);
