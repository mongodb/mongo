// Confirms the $planCacheStats output format includes information about failed plans.
(function() {
"use strict";

load("jstests/libs/sbe_util.js");  // For checkSBEEnabled.

const conn = MongoRunner.runMongod();
assert.neq(null, conn, "mongod was unable to start up");
const testDB = conn.getDB("jstests_plan_cache_list_failed_plans");
const coll = testDB.test;

if (checkSBEEnabled(testDB, ["featureFlagSbeFull"])) {
    jsTest.log("Skipping test because SBE is fully enabled");
    MongoRunner.stopMongod(conn);
    return;
}

coll.drop();

// Setup the database such that it will generate a failing plan and a succeeding plan.
const numDocs = 32;
const smallNumber = 10;
assert.commandWorked(testDB.adminCommand(
    {setParameter: 1, internalQueryMaxBlockingSortMemoryUsageBytes: smallNumber}));
assert.commandWorked(testDB.adminCommand({setParameter: 1, allowDiskUseByDefault: false}));
for (let i = 0; i < numDocs * 2; ++i)
    assert.commandWorked(coll.insert({a: ((i >= (numDocs * 2) - smallNumber) ? 1 : 0), d: i}));

// Create the indexes to create competing plans.
assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.createIndex({d: 1}));

// Assert that the find command found documents.
assert.eq(smallNumber, coll.find({a: 1}).sort({d: 1}).itcount());

// We expect just one plan cache entry.
const planCacheContents = coll.getPlanCache().list();
assert.eq(planCacheContents.length, 1, planCacheContents);
const planCacheEntry = planCacheContents[0];

// There should have been two candidate plans evaluated when the plan cache entry was created.
const creationExecStats = planCacheEntry.creationExecStats;
assert.eq(creationExecStats.length, 2, planCacheEntry);
// We expect that the first plan succeed, and the second failed.
assert(!creationExecStats[0].hasOwnProperty("failed"), planCacheEntry);
assert.eq(creationExecStats[1].failed, true, planCacheEntry);

// The failing plan should have a score of 0.
const candidatePlanScores = planCacheEntry.candidatePlanScores;
assert.eq(candidatePlanScores.length, 2, planCacheEntry);
assert.eq(candidatePlanScores[1], 0, planCacheEntry);

MongoRunner.stopMongod(conn);
})();
