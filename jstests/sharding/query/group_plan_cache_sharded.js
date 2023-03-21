/**
 * Test which verifies that the SBE plan cache correctly caches different plans for $group depending
 * on whether the results will be fed into a merging pipeline or not.
 *
 * @tags: [
 *   requires_sharding,
 *   # The SBE plan cache was enabled by default in 6.3.
 *   requires_fcv_63,
 *   # This test uses the _id index
 *   expects_explicit_underscore_id_index,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/sbe_util.js");  // For 'checkSBEEnabled'.

const st = new ShardingTest({shards: 2, rs: {nodes: 1}});
const mongosDB = st.s.getDB(jsTestName());

// This test is specifically verifying the behavior of the SBE plan cache, which is only enabled
// when SBE is enabled.
if (!checkSBEEnabled(mongosDB)) {
    jsTestLog("Skipping test because SBE is not enabled");
    st.stop();
    return;
}

const collName = jsTestName();
const coll = mongosDB[collName];

function runPipeline(predicateValue) {
    return coll
        .aggregate([
            {$match: {_id: {$gte: predicateValue}}},
            {$group: {_id: null, sumResult: {$sum: "$a"}}}
        ])
        .toArray();
}

// Shard coll on _id.
st.shardColl(
    coll, {_id: 1} /* key */, {_id: 0} /* split */, {_id: 0} /* move */, mongosDB.getName());
const docs = [{_id: -2, a: 1}, {_id: 2, a: 2}];
assert.commandWorked(coll.insertMany(docs));

assert.eq(0, coll.getPlanCache().list().length, "Expected 0 cache entries");

// Run the first aggregate, which will match the second document and target a single shard.
let res = runPipeline(0);
assert.eq(res.length, 1);
assert.eq(res[0], {_id: null, sumResult: 2}, res);

let cacheEntries = coll.getPlanCache().list();
assert.eq(1, cacheEntries.length, cacheEntries);

// Capture the plan cache key from our lone cache entry.
const nonMergingCacheEntry = cacheEntries[0];
assert(nonMergingCacheEntry.hasOwnProperty("planCacheKey"));
const nonMergingCacheKey = nonMergingCacheEntry.planCacheKey;

// Run the second aggregate, which will match both documents and target both shards.
res = runPipeline(-2000);
assert.eq(res.length, 1);
assert.eq(res[0], {_id: null, sumResult: 3}, res);

// The second aggregate will produce two additional cache entries; one for each shard.
cacheEntries = coll.getPlanCache().list();
assert.eq(3, cacheEntries.length, cacheEntries);

// Verify that all three cache entries are pinned and active. Then, verify that exactly one
// cache entry has the 'nonMergingCacheKey', while the other two have a different key.
let mergingCacheKey = null;
let mergingKeyCount = 0;
let nonMergingKeyCount = 0;
for (const cacheEntry of cacheEntries) {
    assert(cacheEntry.isPinned, cacheEntry);
    assert(cacheEntry.isActive, cacheEntry);
    assert(cacheEntry.hasOwnProperty("planCacheKey"));
    if (cacheEntry.planCacheKey === nonMergingCacheKey) {
        nonMergingKeyCount++;
    } else {
        mergingKeyCount++;
        // If we haven't seen the merging cache key before, stash it so that we can verify that the
        // two merging plans have the exact same cache key.
        if (mergingCacheKey === null) {
            mergingCacheKey = cacheEntry.planCacheKey;
        } else {
            assert.eq(cacheEntry.planCacheKey, mergingCacheKey, tojson(cacheEntries));
        }
    }
}

assert.eq(nonMergingKeyCount, 1, tojson(cacheEntries));
assert.eq(mergingKeyCount, 2, tojson(cacheEntries));

st.stop();
}());
