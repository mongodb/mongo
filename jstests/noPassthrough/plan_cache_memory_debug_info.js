/**
 * Tests that detailed debug information is excluded from new plan cache entries once the estimated
 * cumulative size of the system's plan caches exceeds a pre-configured threshold.
 */
(function() {
"use strict";
load("jstests/libs/sbe_util.js");  // For checkSBEEnabled.

/**
 * Creates two indexes for the given collection. In order for plans to be cached, there need to be
 * at least two possible indexed plans.
 */
function createIndexesForColl(coll) {
    assert.commandWorked(coll.createIndex({a: 1}));
    assert.commandWorked(coll.createIndex({b: 1}));
}

function totalPlanCacheSize() {
    const serverStatus = assert.commandWorked(db.serverStatus());
    return serverStatus.metrics.query.planCacheTotalSizeEstimateBytes;
}

function planCacheContents(coll) {
    return coll.aggregate([{$planCacheStats: {}}]).toArray();
}

/**
 * Retrieve the cache entry associated with the query shape defined by the given 'filter' (assuming
 * the query has no projection, sort, or collation). Asserts that a plan cache entry with the
 * expected key is present in the $planCacheStats output, and returns the matching entry.
 */
function getPlanCacheEntryForFilter(coll, filter) {
    // First, use explain to obtain the 'planCacheKey' associated with 'filter'.
    const explain = coll.find(filter).explain();
    const cacheKey = explain.queryPlanner.planCacheKey;
    const allPlanCacheEntries =
        coll.aggregate([{$planCacheStats: {}}, {$match: {planCacheKey: cacheKey}}]).toArray();
    // There should be only one cache entry with the given key.
    assert.eq(allPlanCacheEntries.length, 1, allPlanCacheEntries);
    return allPlanCacheEntries[0];
}

function assertExistenceOfRequiredCacheEntryFields(entry) {
    assert(entry.hasOwnProperty("version"), entry);
    assert.eq(entry["version"], "1", entry);
    assert(entry.hasOwnProperty("queryHash"), entry);
    assert(entry.hasOwnProperty("planCacheKey"), entry);
    assert(entry.hasOwnProperty("isActive"), entry);
    assert(entry.hasOwnProperty("works"), entry);
    assert(entry.hasOwnProperty("timeOfCreation"), entry);
    assert(entry.hasOwnProperty("indexFilterSet"), entry);
    assert(entry.hasOwnProperty("estimatedSizeBytes"), entry);
}

const debugInfoFields =
    ["createdFromQuery", "cachedPlan", "creationExecStats", "candidatePlanScores"];

function assertCacheEntryHasDebugInfo(entry) {
    assertExistenceOfRequiredCacheEntryFields(entry);
    for (const field of debugInfoFields) {
        assert(entry.hasOwnProperty(field), entry);
    }
}

function assertCacheEntryIsMissingDebugInfo(entry) {
    assertExistenceOfRequiredCacheEntryFields(entry);
    for (const field of debugInfoFields) {
        assert(!entry.hasOwnProperty(field), entry);
    }

    // We expect cache entries to be reasonably small when their debug info is stripped. Although
    // there are no strict guarantees on the size of the entry, we can expect that the size estimate
    // should always remain under 2kb.
    assert.lt(entry.estimatedSizeBytes, 2 * 1024, entry);
}

const conn = MongoRunner.runMongod({});
assert.neq(conn, null, "mongod failed to start");
const db = conn.getDB("test");
const coll = db.plan_cache_memory_debug_info;

if (checkSBEEnabled(db, ["featureFlagSbeFull"])) {
    jsTest.log("Skipping test because SBE is fully enabled");
    MongoRunner.stopMongod(conn);
    return;
}

coll.drop();
createIndexesForColl(coll);

const smallQuery = {
    a: 1,
    b: 1,
};

// Create a plan cache entry, and verify that the estimated plan cache size has increased.
let oldPlanCacheSize = totalPlanCacheSize();
assert.eq(0, coll.find(smallQuery).itcount());
let newPlanCacheSize = totalPlanCacheSize();
assert.gt(newPlanCacheSize, oldPlanCacheSize);

// Verify that the cache now has a single entry whose estimated size explains the increase in the
// total plan cache size reported by serverStatus(). The cache entry should contain all expected
// debug info.
let cacheContents = planCacheContents(coll);
assert.eq(cacheContents.length, 1, cacheContents);
const cacheEntry = cacheContents[0];
assertCacheEntryHasDebugInfo(cacheEntry);
assert.eq(cacheEntry.estimatedSizeBytes, newPlanCacheSize - oldPlanCacheSize, cacheEntry);

// Configure the server so that new plan cache entries should not preserve debug info.
const setParamRes = assert.commandWorked(
    db.adminCommand({setParameter: 1, internalQueryCacheMaxSizeBytesBeforeStripDebugInfo: 0}));
const stripDebugInfoThresholdDefault = setParamRes.was;

// Generate a query which includes a 10,000 element $in predicate.
const kNumInElements = 10 * 1000;
const largeQuery = {
    a: 1,
    b: 1,
    c: {$in: Array.from({length: kNumInElements}, (_, i) => i)},
};

// Create a new cache entry using the query with the large $in predicate. Verify that the estimated
// total plan cache size has increased again, and check that there are now two entries in the cache.
oldPlanCacheSize = totalPlanCacheSize();
assert.eq(0, coll.find(largeQuery).itcount());
newPlanCacheSize = totalPlanCacheSize();
assert.gt(newPlanCacheSize, oldPlanCacheSize);
cacheContents = planCacheContents(coll);
assert.eq(cacheContents.length, 2, cacheContents);

// The cache entry associated with 'smallQuery' should retain its debug info, whereas the cache
// entry associated with 'largeQuery' should have had its debug info stripped.
const smallQueryCacheEntry = getPlanCacheEntryForFilter(coll, smallQuery);
assertCacheEntryHasDebugInfo(smallQueryCacheEntry);
let largeQueryCacheEntry = getPlanCacheEntryForFilter(coll, largeQuery);
assertCacheEntryIsMissingDebugInfo(largeQueryCacheEntry);

// The second cache entry should be smaller than the first, despite the query being much larger.
assert.lt(largeQueryCacheEntry.estimatedSizeBytes,
          smallQueryCacheEntry.estimatedSizeBytes,
          cacheContents);

// The new cache entry's size should account for the latest observed increase in total plan cache
// size.
assert.eq(
    largeQueryCacheEntry.estimatedSizeBytes, newPlanCacheSize - oldPlanCacheSize, cacheContents);

// Verify that a new cache entry in a different collection also has its debug info stripped. This
// demonstrates that the size threshold applies on a server-wide basis as opposed to on a
// per-collection basis.
const secondColl = db.plan_cache_memory_debug_info_other;
secondColl.drop();
createIndexesForColl(secondColl);

// Introduce a new cache entry in the second collection's cache and verify that the cumulative plan
// cache size has increased.
oldPlanCacheSize = totalPlanCacheSize();
assert.eq(0, secondColl.find(smallQuery).itcount());
newPlanCacheSize = totalPlanCacheSize();
assert.gt(newPlanCacheSize, oldPlanCacheSize);

// Ensure that the second collection's cache now has one entry, and that entry's debug info is
// stripped.
cacheContents = planCacheContents(secondColl);
assert.eq(cacheContents.length, 1, cacheContents);
assertCacheEntryIsMissingDebugInfo(cacheContents[0]);

// Meanwhile, the contents of the original collection's plan cache should remain unchanged.
cacheContents = planCacheContents(coll);
assert.eq(cacheContents.length, 2, cacheContents);
assertCacheEntryHasDebugInfo(getPlanCacheEntryForFilter(coll, smallQuery));
assertCacheEntryIsMissingDebugInfo(getPlanCacheEntryForFilter(coll, largeQuery));

// Restore the threshold for stripping debug info to its default. Verify that if we add a third
// cache entry to the original collection 'coll', the plan cache size increases once again, and the
// new cache entry stores debug info.
assert.commandWorked(db.adminCommand({
    setParameter: 1,
    internalQueryCacheMaxSizeBytesBeforeStripDebugInfo: stripDebugInfoThresholdDefault,
}));
const smallQuery2 = {
    a: 1,
    b: 1,
    c: 1,
};
oldPlanCacheSize = totalPlanCacheSize();
assert.eq(0, coll.find(smallQuery2).itcount());
newPlanCacheSize = totalPlanCacheSize();
assert.gt(newPlanCacheSize, oldPlanCacheSize);

// Verify that there are now three cache entries.
cacheContents = planCacheContents(coll);
assert.eq(cacheContents.length, 3, cacheContents);

// Make sure that the cache entries have or are missing debug info as expected.
assertCacheEntryHasDebugInfo(getPlanCacheEntryForFilter(coll, smallQuery));
assertCacheEntryHasDebugInfo(getPlanCacheEntryForFilter(coll, smallQuery2));
assertCacheEntryIsMissingDebugInfo(getPlanCacheEntryForFilter(coll, largeQuery));
assertCacheEntryIsMissingDebugInfo(getPlanCacheEntryForFilter(secondColl, smallQuery));

// Clear the cache entry for 'largeQuery' and regenerate it. The cache should grow larger, since the
// regenerated cache entry should now contain debug info. Also, check that the size of the new cache
// entry is estimated to be at least 10kb, since the query itself is known to be at least 10kb.
oldPlanCacheSize = totalPlanCacheSize();
assert.commandWorked(coll.runCommand("planCacheClear", {query: largeQuery}));
cacheContents = planCacheContents(coll);
assert.eq(cacheContents.length, 2, cacheContents);

assert.eq(0, coll.find(largeQuery).itcount());
cacheContents = planCacheContents(coll);
assert.eq(cacheContents.length, 3, cacheContents);

newPlanCacheSize = totalPlanCacheSize();
assert.gt(newPlanCacheSize, oldPlanCacheSize);

largeQueryCacheEntry = getPlanCacheEntryForFilter(coll, largeQuery);
assertCacheEntryHasDebugInfo(largeQueryCacheEntry);
assert.gt(largeQueryCacheEntry.estimatedSizeBytes, 10 * 1024, largeQueryCacheEntry);

MongoRunner.stopMongod(conn);
}());
