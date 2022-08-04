/**
 * The SBE and classic plan caches use the same serverStatus metric to track the cumulative size.
 * The classic cache uses a special mechanism to reduce memory footprint by stripping debug info
 * from plan cache entries when a certain threshold is reached. We need to make sure that when the
 * threshold is reached by adding entries to the SBE plan cache, the classic cache will start
 * stripping debug info even though the size of the classic cache may be below the threshold.
 */

(function() {
"use strict";

load("jstests/libs/analyze_plan.js");  // For getPlanCacheKeyFromShape
load("jstests/libs/sbe_util.js");      // For checkSBEEnabled.

const conn = MongoRunner.runMongod({});
assert.neq(conn, null, "mongod failed to start");
const db = conn.getDB("sbe_plan_cache_memory_debug_info");

if (!checkSBEEnabled(db, ["featureFlagSbeFull"])) {
    jsTest.log("Skipping test because SBE is not fully enabled");
    MongoRunner.stopMongod(conn);
    return;
}

function createTestCollection(collectionName) {
    const coll = db[collectionName];
    coll.drop();
    // Create multiple indexes to ensure we go through the multi-planner.
    assert.commandWorked(coll.createIndexes([{a: 1}, {b: 1}, {a: 1, b: 1}, {b: 1, a: 1}]));
    return coll;
}

function getPlanCacheEntryForQueryShape(coll, queryShape) {
    const planCacheKey = getPlanCacheKeyFromShape({query: queryShape, collection: coll, db});
    const allPlanCacheEntries =
        coll.aggregate([{$planCacheStats: {}}, {$match: {planCacheKey}}]).toArray();
    assert.eq(allPlanCacheEntries.length, 1, allPlanCacheEntries);
    return allPlanCacheEntries[0];
}

const debugInfoFields =
    ["createdFromQuery", "cachedPlan", "creationExecStats", "candidatePlanScores"];

function assertCacheEntryHasDebugInfo(coll, queryShape) {
    const entry = getPlanCacheEntryForQueryShape(coll, queryShape);
    for (const field of debugInfoFields) {
        assert(entry.hasOwnProperty(field), entry);
    }
}

function assertCacheEntryIsMissingDebugInfo(coll, queryShape) {
    const entry = getPlanCacheEntryForQueryShape(coll, queryShape);
    for (const field of debugInfoFields) {
        assert(!entry.hasOwnProperty(field), entry);
    }
}

function getPlanCacheSize() {
    return db.runCommand({serverStatus: 1}).metrics.query.planCacheTotalSizeEstimateBytes;
}

// Set a large value to internalQueryCacheMaxSizeBytesBeforeStripDebugInfo to make sure that Debug
// Info wouldn't be stripped off.
assert.commandWorked(db.adminCommand(
    {setParameter: 1, internalQueryCacheMaxSizeBytesBeforeStripDebugInfo: 536870912}));

const initialPlanCacheSize = getPlanCacheSize();

// Add some entries to SBE Plan Cache and make sure that the global planCacheSize metric is affected
// by the inserted entries.
const sbeColl = createTestCollection("sbe");
assert.eq(0, sbeColl.find({a: 0}).itcount());
assert.eq(0, sbeColl.find({a: 2, b: 4}).itcount());
const planCacheSizeAfterSbeStep = getPlanCacheSize();
assert.lt(initialPlanCacheSize, planCacheSizeAfterSbeStep);

// Force classic plan cache.
assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryForceClassicEngine: true}));

// Create a new collection for classic queries so we can easily assess its plan cache.
const classicColl = createTestCollection("classic");

// Insert an entry to Classic Plan Cache and make sure that the global planCacheSize metric is
// affected by the inserted entry as well as the entry contains DebugInfo.
assert.eq(0, classicColl.find({a: 0}).itcount());
assertCacheEntryHasDebugInfo(classicColl, {a: 0});
const planCacheSizeAfterClassicStep = getPlanCacheSize();
assert.lt(planCacheSizeAfterSbeStep, planCacheSizeAfterClassicStep);

// Set a smaller internalQueryCacheMaxSizeBytesBeforeStripDebugInfo to make sure that next inserted
// entry should be stripped of the debug info.
assert.commandWorked(db.adminCommand({
    setParameter: 1,
    internalQueryCacheMaxSizeBytesBeforeStripDebugInfo: planCacheSizeAfterClassicStep
}));

// Insert a new entry to Classic Plan Cache and asserts that it does not have the debug info.
assert.eq(0, classicColl.find({a: 2, b: 4}).itcount());
assertCacheEntryIsMissingDebugInfo(classicColl, {a: 2, b: 4});

MongoRunner.stopMongod(conn);
}());
