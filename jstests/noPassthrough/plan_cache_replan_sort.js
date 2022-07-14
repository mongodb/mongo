/**
 * Test that when replanning happens due to blocking sort's memory limit, we include
 * replanReason: "cached plan returned: ..." in the profiling data.
 * @tags: [
 *   requires_profiling,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");
load("jstests/libs/profiler.js");
load("jstests/libs/sbe_util.js");  // For checkSBEEnabled.

const conn = MongoRunner.runMongod({setParameter: {allowDiskUseByDefault: false}});
const db = conn.getDB("test");
const coll = db.plan_cache_replan_sort;
coll.drop();

// Ensure a plan with a sort stage gets cached.
assert.commandWorked(coll.createIndex({x: 1}));
assert.commandWorked(coll.createIndex({y: 1}));
const docs = Array.from({length: 20}, (_, i) => ({x: 7, y: i}));
assert.commandWorked(coll.insert(docs));
// Insert an extra document such that the initial query has a single document to sort.
assert.commandWorked(coll.insert({x: 5, y: 1}));

// Set the memory limit to be large enough to sort a single document in the collection.
const documentBsonSize = Object.bsonsize(docs[0]);
const sizeMultiplier = 5.0;
assert.commandWorked(db.adminCommand({
    setParameter: 1,
    internalQueryMaxBlockingSortMemoryUsageBytes: documentBsonSize * sizeMultiplier
}));

// { x: 5 } should match a single document and sort it, which should be within the sort memory
// limit.
assert.eq(1, coll.find({x: 5}).sort({y: 1}).itcount());
// We need to run the query twice for it to be marked "active" in the plan cache.
assert.eq(1, coll.find({x: 5}).sort({y: 1}).itcount());

const cachedPlans = coll.getPlanCache().list();
assert.eq(1, cachedPlans.length, cachedPlans);
assert.eq(true, cachedPlans[0].isActive, cachedPlans);
const cachedPlan = getCachedPlan(cachedPlans[0].cachedPlan);
const cachedPlanVersion = cachedPlans[0].version;
if (checkSBEEnabled(db, ["featureFlagSbeFull"])) {
    // If the SBE plan cache is on, then the cached plan has a different format.
    assert.eq(cachedPlanVersion, "2", cachedPlans);
    assert(cachedPlan.stages.includes("sort"), cachedPlans);
} else {
    assert.eq(cachedPlanVersion, "1", cachedPlans);
    assert.eq(cachedPlan.stage, "SORT", cachedPlans);
}

// Assert we "replan", by running the same query with different parameters. This time the filter is
// not selective at all and will result in more documents attempted to be sorted.
assert.commandWorked(db.setProfilingLevel(2));
assert.eq(20, coll.find({x: 7}).sort({y: 1}).itcount());

const profileObj = getLatestProfilerEntry(db, {op: "query"});
assert.eq(profileObj.ns, coll.getFullName(), profileObj);
assert.eq(profileObj.replanned, true, profileObj);
assert.eq(
    profileObj.replanReason,
    `cached plan returned: QueryExceededMemoryLimitNoDiskUseAllowed: Sort exceeded memory limit of ${
        documentBsonSize * sizeMultiplier} bytes, but did not opt in to external sorting.`,
    profileObj);

MongoRunner.stopMongod(conn);
}());
