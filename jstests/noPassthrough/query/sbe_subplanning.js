/**
 * Test that SBE-eligible plans which use subplanning are correctly cached, both with the SBE plan
 * cache and classic plan cache.
 * @tags: [
 *   requires_profiling,
 * ]
 */
import {assertCacheUsage} from "jstests/libs/plan_cache_utils.js";
import {getLatestProfilerEntry} from "jstests/libs/profiler.js";
import {
    checkSbeFullFeatureFlagEnabled,
    checkSbeRestrictedOrFullyEnabled,
} from "jstests/libs/sbe_util.js";

const conn = MongoRunner.runMongod();
const db = conn.getDB("test");
const coll = db.sbe_subplanning;
coll.drop();

const sbeEnabled = checkSbeRestrictedOrFullyEnabled(db);
const sbePlanCacheEnabled = checkSbeFullFeatureFlagEnabled(db);
assert.commandWorked(db.setProfilingLevel(2));

if (!sbeEnabled) {
    jsTestLog("Skipping test when SBE is disabled");
    MongoRunner.stopMongod(conn);
    quit();
}

for (let i = 0; i < 100; i++) {
    assert.commandWorked(coll.insert({a: 1, b: i, c: 1, d: i}));
}
assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.createIndex({b: 1}));
assert.commandWorked(coll.createIndex({c: 1}));
assert.commandWorked(coll.createIndex({d: 1}));

const orFilter = {
    $or: [{a: 1, b: 1}, {c: 1, d: 1}]
};

const pipeline = [{$match: orFilter}, {$group: {_id: null, s: {$sum: "$a"}}}];
const queryPlanner = coll.explain().aggregate(pipeline).queryPlanner;

// Sub queries created from each branch of the $or. The $group at the end is just to make the query
// SBE compatible. When the classic cache is being used, we want to guarantee that these queries do
// NOT share a cache entry with the cache entries generated for sub planning.
const subQuery1 = [{$match: {a: 1, b: 1}}, {$group: {_id: 1, s: {$sum: "$a"}}}];
const subQuery1Planner = coll.explain().aggregate(subQuery1).queryPlanner;
const subQuery2 = [{$match: {c: 1, d: 1}}, {$group: {_id: 1, s: {$sum: "$a"}}}];
const subQuery2Planner = coll.explain().aggregate(subQuery2).queryPlanner;

function assertOneResult(cursor) {
    let res = cursor.toArray();
    assert.eq(res.length, 1);
}

// First make sure there's no matching entries in the plan cache.
{
    const planCacheEntries = coll.aggregate([
                                     {$planCacheStats: {}},
                                     {$match: {planCacheShapeHash: queryPlanner.planCacheShapeHash}}
                                 ])
                                 .toArray();
    assert.eq(planCacheEntries.length, 0);
}

// Now run the query the first time. We do not expect the cache to be used, though we do expect
// cache entrie(s) to be created.
{
    assertOneResult(coll.aggregate(pipeline));

    const profileObj = getLatestProfilerEntry(db, {op: {$in: ["command"]}, ns: coll.getFullName()});

    assert.eq(profileObj.nreturned, 1);
    assert.eq(profileObj.planCacheShapeHash, queryPlanner.planCacheShapeHash);
    assert.eq(profileObj.planCacheKey, queryPlanner.planCacheKey);
    assert.eq(profileObj.queryFramework, "sbe");
    // Check that the planSummary is two IXSCANs.
    assert.eq(profileObj.planSummary.match(/IXSCAN/g).length, 2);

    // Regardless of which cache is used, the first run should not be cached.
    assert(!profileObj.fromPlanCache);

    let planCacheEntries = coll.getPlanCache().list();

    if (sbePlanCacheEnabled) {
        // We should expect a pinned cache entry to get written for the entire query.
        planCacheEntries = planCacheEntries.filter((entry) => entry.planCacheShapeHash ==
                                                       queryPlanner.planCacheShapeHash);

        assert.eq(planCacheEntries.length, 1);
        const entry = planCacheEntries[0];
        assert.eq(entry.planCacheKey, queryPlanner.planCacheKey, entry);
        assert.eq(entry.planCacheShapeHash, queryPlanner.planCacheShapeHash, entry);
        assert.eq(entry.isActive, true, entry);
        assert.eq(entry.version, "2", entry);
        assert.eq(entry.works, 0);
        assert(!("worksType" in entry));
        assert.eq(entry.isPinned, true);
    } else {
        // No version:2 entries should have been written.
        assert.eq(planCacheEntries.filter((entry) => entry.version === "2").length, 0);

        // We should have two cache entries.
        assert.eq(planCacheEntries.length, 2);

        // Ensure that neither cache entry matches the sub-query.
        assert.eq(planCacheEntries.filter((entry) =>
                                              entry.planCacheKey == subQuery1Planner.planCacheKey ||
                                              entry.planCacheKey == subQuery2Planner.planCacheKey),
                  0);

        // Both entries should be inactive.
        assert.eq(planCacheEntries.filter((entry) => entry.isActive).length, 0);
        // Both entries store 'works'.
        assert.eq(planCacheEntries.filter((entry) => entry.worksType == "works").length, 2);
    }
}

// Run the query a second time.
{
    assertOneResult(coll.aggregate(pipeline));
    const profileObj = getLatestProfilerEntry(db, {op: {$in: ["command"]}, ns: coll.getFullName()});

    assert.eq(profileObj.nreturned, 1);
    assert.eq(profileObj.planCacheShapeHash, queryPlanner.planCacheShapeHash);
    assert.eq(profileObj.planCacheKey, queryPlanner.planCacheKey);
    assert.eq(profileObj.queryFramework, "sbe");

    // Check that the planSummary is two IXSCANs.
    assert.eq(profileObj.planSummary.match(/IXSCAN/g).length, 2);

    let planCacheEntries = coll.getPlanCache().list();

    if (sbePlanCacheEnabled) {
        // The pinned cache entry from the first run should still exist and should have been used
        // to answer the query this time.
        planCacheEntries = planCacheEntries.filter((entry) => entry.planCacheShapeHash ==
                                                       queryPlanner.planCacheShapeHash);

        assert(profileObj.fromPlanCache);

        assert.eq(planCacheEntries.length, 1);
        const entry = planCacheEntries[0];
        assert.eq(entry.planCacheKey, queryPlanner.planCacheKey, entry);
        assert.eq(entry.isActive, true, entry);
        assert.eq(entry.version, 2, entry);
        assert.eq(entry.works, 0);
        assert(!("worksType" in entry));
        assert.eq(entry.isPinned, true);
    } else {
        // No version:2 entries should have been written.
        assert.eq(planCacheEntries.filter((entry) => entry.version == 2).length, 0);

        // We should still have two cache entries.
        assert.eq(planCacheEntries.length, 2);

        // Now both entries should be active, because they were used to answer the query
        // successfully.
        assert.eq(planCacheEntries.filter((entry) => entry.isActive).length, 2);
        // Both entries store 'works'.
        assert.eq(planCacheEntries.filter((entry) => entry.worksType == "works").length, 2);
    }
}

// Run the query a third time.
{
    assertOneResult(coll.aggregate(pipeline));

    const profileObj = getLatestProfilerEntry(db, {op: {$in: ["command"]}, ns: coll.getFullName()});

    assert.eq(profileObj.nreturned, 1);
    assert.eq(profileObj.planCacheShapeHash, queryPlanner.planCacheShapeHash);
    assert.eq(profileObj.planCacheKey, queryPlanner.planCacheKey);
    assert.eq(profileObj.queryFramework, "sbe");

    // Check that the planSummary is two IXSCANs.
    assert.eq(profileObj.planSummary.match(/IXSCAN/g).length, 2);

    let planCacheEntries = coll.getPlanCache().list();

    if (sbePlanCacheEnabled) {
        assert(profileObj.fromPlanCache);

        planCacheEntries = planCacheEntries.filter((entry) => entry.planCacheShapeHash ==
                                                       queryPlanner.planCacheShapeHash);

        assert.eq(planCacheEntries.length, 1);
        const entry = planCacheEntries[0];
        assert.eq(entry.planCacheKey, queryPlanner.planCacheKey, entry);
        assert.eq(entry.isActive, true, entry);
        assert.eq(entry.version, 2, entry);
        assert.eq(entry.works, 0);
        assert(!("worksType" in entry));
        assert.eq(entry.isPinned, true);
    } else {
        // What we want to show here is that the query used the two cache entries that were written
        // prior. Checking this directly via a JS test is not possible, so we do the closest we
        // can.  We do check that the cache entries become active, which indicates they were
        // successfully used in the second run.

        // No version:2 entries should have been written.
        assert.eq(planCacheEntries.filter((entry) => entry.version == 2).length, 0);

        // We should still have two cache entries.
        assert.eq(planCacheEntries.length, 2);

        // Now both entries should be active, because they were used to answer the query
        // successfully.
        assert.eq(planCacheEntries.filter((entry) => entry.isActive).length, 2);
        // Both entries store 'works'.
        assert.eq(planCacheEntries.filter((entry) => entry.worksType == "works").length, 2);
    }
}

// Regardless of which cache is being used, running the "sub queries" individually should
// not re-use the cache entries generated earlier.

const cacheEntryVersion = sbePlanCacheEnabled ? 2 : 1;

// Running subQuery1 or subQuery2 requires multi planning and generates an inactive cache entry.
for (let pipe of [subQuery1, subQuery2]) {
    assert.eq(coll.aggregate(pipe).itcount(), 1);
    assertCacheUsage({
        queryColl: coll,
        pipeline: pipe,
        fromMultiPlanning: true,
        cacheEntryVersion: cacheEntryVersion,
        cacheEntryIsActive: false,
        cachedIndexName: null,
    });
}

// A second run results in an active cache entry.
for (let pipe of [subQuery1, subQuery2]) {
    assert.eq(coll.aggregate(pipe).itcount(), 1);
    assertCacheUsage({
        queryColl: coll,
        pipeline: pipe,
        fromMultiPlanning: true,
        cacheEntryVersion: cacheEntryVersion,
        cacheEntryIsActive: true,
        cachedIndexName: null,
    });
}

// A third run can use the active cache entry.
for (let pipe of [subQuery1, subQuery2]) {
    assert.eq(coll.aggregate(pipe).itcount(), 1);
    assertCacheUsage({
        queryColl: coll,
        pipeline: pipe,
        fromMultiPlanning: false,
        cacheEntryVersion: cacheEntryVersion,
        cacheEntryIsActive: true,
        cachedIndexName: null,
    });
}

jsTestLog("Running test which forces SubPlanner to plan the entire query");

// Now we run a query where the planner attempts to use subplanning, but ends up planning the whole
// query as one.
{
    coll.dropIndexes();
    assert.commandWorked(coll.createIndex({a: 1}));
    assert.commandWorked(coll.createIndex({b: 1}));
    assert.commandWorked(coll.createIndex({x: 1}));

    function checkProfilerAndCache({isActive, isPinned, fromPlanCache, worksType}) {
        const profileObj =
            getLatestProfilerEntry(db, {op: {$in: ["command", "query"]}, ns: coll.getFullName()});

        if (fromPlanCache) {
            assert.eq(profileObj.fromPlanCache, true, profileObj);
        } else {
            // x:1 index was used.
            assert.eq(/x: 1/g.test(profileObj.planSummary), true);
            assert(!("fromPlanCache" in profileObj));
        }

        const engineUsed = profileObj.queryFramework;

        const cacheEntries = coll.aggregate([
                                     {$planCacheStats: {}},
                                     {$match: {planCacheShapeHash: profileObj.planCacheShapeHash}}
                                 ])
                                 .toArray();
        assert.eq(cacheEntries.length, 1);
        const cacheEntry = cacheEntries[0];

        if (engineUsed == "sbe") {
            assert.eq(cacheEntry.planCacheShapeHash, profileObj.planCacheShapeHash);
            assert.eq(cacheEntry.planCacheKey, profileObj.planCacheKey);
            assert.eq(cacheEntry.isActive, isActive);

            // It must be tracking reads if the query ran in SBE.
            if (isPinned) {
                assert(!("worksType" in cacheEntry));
            } else {
                assert.eq(cacheEntry.worksType, "reads");
            }

            if (sbePlanCacheEnabled) {
                assert.eq(cacheEntry.version, "2");
            } else {
                assert.eq(cacheEntry.version, "1");
            }
        } else {
            // There should be a cache entry tracking 'works'.
            assert.eq(cacheEntry.version, "1");
            assert.eq(cacheEntry.worksType, "works");
        }
    }

    const kFilter = {$or: [{a: 1}, {b: 1}]};

    // First run the query as a simple find command.
    {
        // First run.
        assert.eq(coll.find(kFilter).sort({x: 1}).itcount(), 100);
        checkProfilerAndCache({
            // When the SBE plan cache is used, the entry will be pinned and enabled immediately.
            isActive: sbePlanCacheEnabled,
            isPinned: sbePlanCacheEnabled,
            fromPlanCache: false
        });

        // Second run.
        assert.eq(coll.find(kFilter).sort({x: 1}).itcount(), 100);
        checkProfilerAndCache(
            {isActive: true, isPinned: sbePlanCacheEnabled, fromPlanCache: sbePlanCacheEnabled});

        // Third run.
        assert.eq(coll.find(kFilter).sort({x: 1}).itcount(), 100);
        checkProfilerAndCache({isActive: true, isPinned: sbePlanCacheEnabled, fromPlanCache: true});
    }

    coll.getPlanCache().clear();

    // Now run the same series of tests, but using an SBE-eligible aggregation pipeline.
    {
        const pipe = [
            {$match: kFilter},
            {$sort: {x: 1}},
            // We use an order-sensitive accumulator so that the $sort cannot be removed.
            {$group: {_id: "$b", max: {$push: "$unknownField"}}}
        ];

        // First run.
        assert.eq(coll.aggregate(pipe).itcount(), 100);
        checkProfilerAndCache({
            // When the SBE plan cache is used, the entry will be pinned and enabled immediately.
            isActive: sbePlanCacheEnabled,
            isPinned: sbePlanCacheEnabled,
            fromPlanCache: false
        });

        assert.eq(coll.aggregate(pipe).itcount(), 100);
        checkProfilerAndCache(
            {isActive: true, isPinned: sbePlanCacheEnabled, fromPlanCache: sbePlanCacheEnabled});

        assert.eq(coll.aggregate(pipe).itcount(), 100);
        checkProfilerAndCache({isActive: true, isPinned: sbePlanCacheEnabled, fromPlanCache: true});
    }
}

MongoRunner.stopMongod(conn);
