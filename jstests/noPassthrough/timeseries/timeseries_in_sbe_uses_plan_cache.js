/**
 * Ensures that SBE-eligible time series queries use the classic plan cache.
 */
import {assertCacheUsage} from "jstests/libs/plan_cache_utils.js";
import {checkSbeStatus, kSbeRestricted} from "jstests/libs/sbe_util.js";

// Start a single mongoD using MongoRunner.
const conn = MongoRunner.runMongod();
assert.neq(null, conn, "mongod was unable to start up");

// Create the test DB and collection.
const db = conn.getDB("test");
const collName = jsTestName();
const coll = db[collName];

if (checkSbeStatus(db) !== kSbeRestricted) {
    jsTestLog("Skipping test under configurations other than trySbeRestricted.");
    MongoRunner.stopMongod(conn);
    quit();
}

coll.drop();
assert.commandWorked(db.createCollection(coll.getName(), {
    timeseries: {timeField: 'time', metaField: 'meta'},
}));
const bucketsColl = db.getCollection('system.buckets.' + coll.getName());

// Just after midnight on Saturday, April 8, 2023 in GMT, expressed as milliseconds since the epoch.
const datePrefix = 1680912440;
for (let i = 0; i < 50; ++i) {
    assert.commandWorked(
        coll.insert({_id: i, time: new Date(datePrefix + i * 10), meta: "foobar", x: i, y: i * 2}));
}
assert.gt(bucketsColl.count(), 0);

// assertCacheUsage() requires the profiler.
assert.commandWorked(db.setProfilingLevel(2));

/**
 * Checks that the given pipeline runs and uses an active cache entry. Also checks that the pipeline
 * returns one document with _id equal to 'expectedId'.
 */
function checkPipelineUsesCacheEntry({pipeline, expectedId, cacheEntry}) {
    let results = coll.aggregate(pipeline).toArray();
    assert.eq(results.length, 1);
    assert.eq(results[0]._id, expectedId);

    const newEntry = assertCacheUsage({
        planCacheColl: bucketsColl,
        queryColl: coll,
        pipeline: pipeline,
        fromMultiPlanning: false,
        cacheEntryVersion: 1,
        cacheEntryIsActive: true,
        cachedIndexName: null
    });

    assert.eq(cacheEntry.planCacheShapeHash, newEntry.planCacheShapeHash, {cacheEntry, newEntry});
    assert.eq(cacheEntry.planCacheKey, newEntry.planCacheKey, {cacheEntry, newEntry});
    return newEntry;
}

/**
 * Run the pipeline three times, assert that we have the following plan cache entries:
 *      1. The pipeline runs from the multi-planner, saving an inactive cache entry.
 *      2. The pipeline runs from the multi-planner, activating the cache entry.
 *      3. The pipeline runs from cached solution planner, using the active cache entry.
 *
 * Also checks that the pipeline returns one document whose _id is 'expectedId'.
 */
function testLoweredPipeline({
    queryColl,      // collection to run the query on (may be a view).
    planCacheColl,  // collection to read profiler from. (If not a view, same as queryColl)
    pipeline,
    expectedId
}) {
    const expl = queryColl.explain().aggregate(pipeline);
    let results = queryColl.aggregate(pipeline).toArray();
    assert.eq(results.length, 1, results);
    assert.eq(results[0]._id, expectedId);

    const entry = assertCacheUsage({
        planCacheColl: planCacheColl,
        queryColl: queryColl,
        pipeline: pipeline,
        fromMultiPlanning: true,
        cacheEntryVersion: 1,
        cacheEntryIsActive: false,
        cachedIndexName: null
    });

    results = queryColl.aggregate(pipeline).toArray();
    assert.eq(results.length, 1, results);
    assert.eq(results[0]._id, expectedId);
    let nextEntry = assertCacheUsage({
        planCacheColl: planCacheColl,
        queryColl: queryColl,
        pipeline: pipeline,
        fromMultiPlanning: true,
        cacheEntryVersion: 1,
        cacheEntryIsActive: true,
        cachedIndexName: null
    });

    assert.eq(entry.planCacheShapeHash, nextEntry.planCacheShapeHash, {entry, nextEntry});
    assert.eq(entry.planCacheKey, nextEntry.planCacheKey, {entry, nextEntry});

    // For the last run, we use the checkPipelineUsesCacheEntry() helper.
    nextEntry = checkPipelineUsesCacheEntry({pipeline, expectedId, cacheEntry: entry});

    return nextEntry;
}

// This query has two possible plans: one is IXSCAN/FETCH on the (meta,time) index, and the other
// is a clustered collection scan with bounds on the 'time' field. This ensures that we have
// multiple solutions to multi plan.
const originalPipeline = [
    {
        $match: {
            time: {$gt: new Date(datePrefix), $lt: new Date(datePrefix + 500)},
            meta: "foobar",
            x: {$eq: 20}
        }
    },
    {$project: {_id: 1, x: 1}}
];

const cacheEntry = testLoweredPipeline(
    {queryColl: coll, planCacheColl: bucketsColl, pipeline: originalPipeline, expectedId: 20});

// Now run pipelines with the same shape but different parameters on 'time' and 'x'. These
// should re-use the cache entry and not require replanning.
{
    const pipelineWithDifferentTimeFilter = [
        {
            $match: {
                time: {
                    $gt: new Date(datePrefix),
                    $lt: new Date(datePrefix + 400 /* Different from above. */)
                },
                meta: "foobar",
                x: {$eq: 20}
            }
        },
        {$project: {_id: 1, x: 1}}
    ];
    checkPipelineUsesCacheEntry(
        {pipeline: pipelineWithDifferentTimeFilter, expectedId: 20, cacheEntry});

    const pipelineWithDifferentXFilter = [
        {
            $match: {
                time: {$gt: new Date(datePrefix), $lt: new Date(datePrefix + 500)},
                meta: "foobar",
                x: {$eq: 21 /* Different from above */}
            }
        },
        {$project: {_id: 1, x: 1}}
    ];
    checkPipelineUsesCacheEntry(
        {pipeline: pipelineWithDifferentXFilter, expectedId: 21, cacheEntry});
}

{
    bucketsColl.getPlanCache().clear();

    // Now run a pipeline with a different shape, by adding a predicate on 'y'. This
    // should result in a cache entry with a different planCacheKey.
    const pipelineWithDifferentShape = [
        {
            $match: {
                time: {$gt: new Date(datePrefix), $lt: new Date(datePrefix + 500)},
                meta: "foobar",
                x: 20,
                y: {$gt: 0}
            }
        },
        {$project: {_id: 1, x: 1}}
    ];
    const newCacheEntry = testLoweredPipeline({
        queryColl: coll,
        planCacheColl: bucketsColl,
        pipeline: pipelineWithDifferentShape,
        expectedId: 20
    });
    assert.neq(newCacheEntry.planCacheKey, cacheEntry.planCacheKey);
}

MongoRunner.stopMongod(conn);
