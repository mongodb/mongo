/**
 * Test that when replanning happens due to blocking sort's memory limit, we include
 * replanReason: "cached plan returned: ..." in the profiling data.
 * @tags: [
 *   requires_profiling,
 *   sbe_incompatible,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/profiler.js");

const conn = MongoRunner.runMongod({
    // Disallow blocking sort, by setting the memory limit very low (1 byte).
    setParameter: "internalQueryMaxBlockingSortMemoryUsageBytes=1"
});
const db = conn.getDB("test");
const coll = db.plan_cache_replan_sort;
coll.drop();

// Ensure a plan with a sort stage gets cached.
assert.commandWorked(coll.createIndex({x: 1}));
assert.commandWorked(coll.createIndex({y: 1}));
const docs = Array.from({length: 20}, (_, i) => ({x: 7, y: i}));
assert.commandWorked(coll.insert(docs));
// { x: 5 } is very selective because it matches 0 documents, so the x index will win.
// This will succeed despite the low memory limit on sort, because the sort stage will see zero
// documents.
assert.eq(0, coll.find({x: 5}).sort({y: 1}).itcount());
// We need to run the query twice for it to be marked "active" in the plan cache.
assert.eq(0, coll.find({x: 5}).sort({y: 1}).itcount());

const cachedPlans = coll.getPlanCache().list();
assert.eq(1, cachedPlans.length, cachedPlans);
assert.eq(true, cachedPlans[0].isActive, cachedPlans);
assert.eq("SORT", cachedPlans[0].cachedPlan.stage, cachedPlans);

// Assert we "replan", by running the same query with different parameters.
// This time the filter is not selective at all.
db.setProfilingLevel(2);
assert.eq(20, coll.find({x: 7}).sort({y: 1}).itcount());

const profileObj = getLatestProfilerEntry(db, {op: "query"});
assert.eq(profileObj.ns, coll.getFullName(), profileObj);
assert.eq(profileObj.replanned, true, profileObj);
assert.eq(
    profileObj.replanReason,
    "cached plan returned: QueryExceededMemoryLimitNoDiskUseAllowed: Sort exceeded memory limit " +
        "of 1 bytes, but did not opt in to external sorting.",
    profileObj);

MongoRunner.stopMongod(conn);
}());
