/**
 * Test that plans with $unwind when lowered to SBE are cached and replanned as appropriate.
 * @tags: [
 *   requires_profiling,
 * ]
 */
import {assertCacheUsage, setUpActiveCacheEntry} from "jstests/libs/plan_cache_utils.js";
import {
    checkSbeFullFeatureFlagEnabled,
    checkSbeFullyEnabled,
} from "jstests/libs/sbe_util.js";

const conn = MongoRunner.runMongod();
const db = conn.getDB("test");
const coll = db.plan_cache_replan_unwind;
coll.drop();

const sbeEnabled = checkSbeFullyEnabled(db);
const sbePlanCacheEnabled = checkSbeFullFeatureFlagEnabled(db);

assert.commandWorked(db.setProfilingLevel(2));

// Carefully construct a collection so that some queries will do well with an {a: 1} index
// and others with a {b: 1} index.
const bigArray = new Array(200).fill(0);
for (let i = 1000; i < 1100; i++) {
    assert.commandWorked(coll.insert({a: 1, b: i, arr: bigArray}));
    assert.commandWorked(coll.insert({a: 1, b: i, arr: bigArray}));
    assert.commandWorked(coll.insert({a: 1, b: i, arr: bigArray}));
}
for (let i = 1000; i < 1100; i++) {
    assert.commandWorked(coll.insert({a: i, b: 1, arr: bigArray}));
    assert.commandWorked(coll.insert({a: i, b: 1, arr: bigArray}));
    assert.commandWorked(coll.insert({a: i, b: 1, arr: bigArray}));
}
assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.createIndex({b: 1}));

function getAssertCount(count) {
    return function assertCount(cursor) {
        assert.eq(count, cursor.itcount());
    };
}

function testFn(
    aIndexPipeline, aExpectedResultCount, bIndexPipeline, bExpectedResultCount, cacheEntryVersion) {
    setUpActiveCacheEntry(coll,
                          aIndexPipeline,
                          cacheEntryVersion,
                          "a_1" /* cachedIndexName */,
                          getAssertCount(aExpectedResultCount));

    // Now run the other pipeline, which has the same query shape but is faster with a different
    // index. It should trigger re-planning of the query.
    assert.eq(bExpectedResultCount, coll.aggregate(bIndexPipeline).itcount());
    assertCacheUsage({
        queryColl: coll,
        pipeline: bIndexPipeline,
        fromMultiPlanning: true,
        cacheEntryVersion,
        cacheEntryIsActive: true,
        cachedIndexName: "b_1"
    });

    assert.eq(bExpectedResultCount, coll.aggregate(bIndexPipeline).itcount());
    assertCacheUsage({
        queryColl: coll,
        pipeline: bIndexPipeline,
        fromMultiPlanning: false,
        cacheEntryVersion,
        cacheEntryIsActive: true,
        cachedIndexName: "b_1"
    });

    coll.getPlanCache().clear();
}

// aIndexPredicate matches 9 documents.
const aIndexPredicate = [{$match: {a: {$gte: 1097, $lt: 1100}, b: {$gte: 1, $lt: 1100}}}];
// When we switch to bIndexPredicate, we are allowed to read ~90 documents before replanning
// happens. It is enough to match one document, so $unwind will turn that into 200 documents. But we
// should still replan, as $unwind should not affect replanning.
const bIndexPredicate = [{$match: {a: {$gte: 1, $lt: 1100}, b: {$gte: 1029, $lt: 1032}}}];

const expectedCacheEntryVersion = sbePlanCacheEnabled ? 2 : 1;

// We add another stage, so $unwind is pushed down.
const unwindSum = [{$unwind: "$arr"}, {$addFields: {sum: {$add: ["$a", "$b", "$arr"]}}}];

const aUnwind = aIndexPredicate.concat(unwindSum);
const bUnwind = bIndexPredicate.concat(unwindSum);

const numExpectedResults = 9 * bigArray.length;
testFn(aUnwind, numExpectedResults, bUnwind, numExpectedResults, expectedCacheEntryVersion);

if (sbeEnabled) {
    // In classic runtime planning for SBE, we should stop trial run if inefficient plan produces
    // too much data to stash.
    const hugeString = Array(1024 * 1024 + 1).toString();  // 1MB of ','
    const foreignColl = db.plan_cache_replan_unwind_foreign;
    assert.commandWorked(foreignColl.createIndex({lookupId: 1}));
    const foreignDoc = {lookupId: 0, str: hugeString};
    for (let i = 0; i < 200; ++i) {
        assert.commandWorked(foreignColl.insertOne(foreignDoc));
    }

    assert.commandWorked(coll.updateMany({}, {$set: {lookupId: 0}}));

    const lookupUnwind = [
        { $lookup: { from: foreignColl.getName(), localField: "lookupId", foreignField: "lookupId", as: "data" } },
        { $unwind: "$data" }
    ];

    setUpActiveCacheEntry(coll,
                          aIndexPredicate.concat(lookupUnwind),
                          expectedCacheEntryVersion /*cacheEntryVersion*/,
                          "a_1" /*cachedIndexName*/,
                          getAssertCount(numExpectedResults));
    assert.eq(numExpectedResults, coll.aggregate(bIndexPredicate.concat(lookupUnwind)).itcount());
    // Assert that replanning did not get triggered (a plan using the {a: 1} index is still in the
    // cache) due to the amount of data that had to get stashed during the trial period growing too
    // large
    assertCacheUsage({
        queryColl: coll,
        pipeline: bIndexPredicate.concat(lookupUnwind),
        fromMultiPlanning: false,
        cacheEntryVersion: expectedCacheEntryVersion,
        cacheEntryIsActive: true,
        cachedIndexName: "a_1"
    });

    coll.getPlanCache().clear();
}

MongoRunner.stopMongod(conn);
