/**
 * Test that replanning caused by a sort memory limit failure does not modify the plan cache.
 *
 * @tags: [
 *   requires_profiling,
 * ]
 */
import {getLatestProfilerEntry} from "jstests/libs/profiler.js";

function assertCacheBehavior(rankingMode) {
    const conn = MongoRunner.runMongod({
        setParameter: {allowDiskUseByDefault: false, internalQueryPlanRanker: rankingMode},
    });
    const db = conn.getDB("test");

    const coll = db[jsTestName()];
    assert(coll.drop());

    assert.commandWorked(coll.createIndex({x: 1}));
    assert.commandWorked(coll.createIndex({y: 1}));
    // 200 documents match {x: 5} and 700 match {x: 7}, evenly spread across the {y: 1} index.
    const pattern = [5, 9, 5, 9, 7, 7, 7, 7, 7, 7, 7];
    const docs = Array.from({length: 1100}, (_, i) => ({x: pattern[i % pattern.length], y: i}));
    assert.commandWorked(coll.insert(docs));

    // Allow sorting the 200 {x: 5} matches but not the 700 {x: 7} matches.
    // The sorter memory per doc is the size of the doc plus the size of the sort
    // key. We use 1.5 as an approximation for the size of the sort key.
    const sorterMemoryPerDoc = 1.5 * Object.bsonsize(docs[0]);
    // The max docs allowed before failure can be anywhere in (200, 700).
    const blockSortMaxBytes = 500 * sorterMemoryPerDoc;
    assert.commandWorked(
        db.adminCommand({
            setParameter: 1,
            internalQueryMaxBlockingSortMemoryUsageBytes: blockSortMaxBytes,
        }),
    );

    // Run the selective query twice so the winning SORT plan is cached and the entry becomes active.
    assert.eq(coll.find({x: 5}).sort({y: 1}).itcount(), 200);
    assert.eq(coll.find({x: 5}).sort({y: 1}).itcount(), 200);
    const cacheEntriesBefore = coll.getPlanCache().list();
    assert.eq(cacheEntriesBefore.length, 1, cacheEntriesBefore);
    const entryBefore = cacheEntriesBefore[0];
    assert(entryBefore.isActive, entryBefore);
    assert.eq(entryBefore.cachedPlan.stage, "SORT", entryBefore);

    // The non-selective query overflows the sort memory limit, failing the cached plan and replanning.
    assert.commandWorked(db.setProfilingLevel(2));
    assert.eq(coll.find({x: 7}).sort({y: 1}).itcount(), 700);
    const profileObj = getLatestProfilerEntry(db, {op: "query"});
    assert(
        profileObj.replanReason?.includes("QueryExceededMemoryLimitNoDiskUseAllowed"),
        "expected a replan caused by the sort memory limit",
        {profileObj},
    );

    // The replanned winner must not have been cached, so the entry is unchanged.
    const cacheEntriesAfter = coll.getPlanCache().list();
    assert.eq(cacheEntriesAfter.length, 1, cacheEntriesAfter);
    const entryAfter = cacheEntriesAfter[0];
    assert(entryAfter.isActive, entryAfter);
    assert.eq(entryBefore.cachedPlan, entryAfter.cachedPlan, entryAfter);
    // If the replanned winner gets cached, the "works" value would be different
    assert.eq(entryBefore.works, entryAfter.works, entryAfter);

    MongoRunner.stopMongod(conn);
}

// Test with all plan ranking modes.
assertCacheBehavior("multiPlanning");
assertCacheBehavior("costBased");
assertCacheBehavior("mixed");
