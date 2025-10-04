/**
 * Test that the bucket unpacking with sorting rewrite is performed when plan is cached or
 * replanned.
 *
 * @tags: [
 *     # The test runs commands that are not allowed with security token: setProfilingLevel.
 *     not_allowed_with_signed_security_token,
 *     # Plan cache stats doesn't support different read concerns.
 *     assumes_read_concern_unchanged,
 *     # Explain of a resolved view must be executed by mongos.
 *     directly_against_shardsvrs_incompatible,
 *     # This complicates aggregation extraction.
 *     do_not_wrap_aggregations_in_facets,
 *     # Refusing to run a test that issues an aggregation command with explain because it may
 *     # return incomplete results if interrupted by a stepdown.
 *     does_not_support_stepdowns,
 *     # We use the profiler to get info in order to force replanning.
 *     requires_profiling,
 *     # We need a timeseries collection.
 *     requires_timeseries,
 *     assumes_balancer_off,
 *     # The test examines the SBE plan cache, which initial sync may change the contents of.
 *     examines_sbe_cache
 * ]
 */
import {getTimeseriesCollForDDLOps} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";
import {getLatestProfilerEntry} from "jstests/libs/profiler.js";
import {getAggPlanStage, getAggPlanStages, getPlanCacheKeyFromExplain} from "jstests/libs/query/analyze_plan.js";

const fields = ["a", "b", "i"];

const addDocs = (coll, numDocs, constants = [1, 1]) => {
    let bulk = coll.initializeUnorderedBulkOp();

    assert.eq(fields.length, 3, fields);
    assert.eq(constants.length, 2, constants);

    // `m.a` & `m.b` will have value constants[i]. `m.i` & `t` will have value `i`.
    // `m.i` is to create separate buckets.
    for (let i = 0; i < numDocs; ++i) {
        let meta = {[fields[0]]: constants[0], [fields[1]]: constants[1], [fields[2]]: i};
        bulk.insert({m: meta, t: new Date(i)});
    }

    assert.commandWorked(bulk.execute());
};
const setupCollection = (coll, collName, numDocs) => {
    coll.drop();
    db.createCollection(collName, {timeseries: {timeField: "t", metaField: "m"}});

    addDocs(coll, numDocs);
};

const collName = jsTestName();
const coll = db[collName];
const stageName = "$_internalBoundedSort";

const testBoundedSorterPlanCache = (sortDirection, indexDirection) => {
    // Setup with a few documents.
    const numDocs = 20;
    setupCollection(coll, collName, numDocs);

    assert.commandWorked(coll.createIndex({"m.a": indexDirection, "m.i": indexDirection, t: indexDirection}));
    assert.commandWorked(coll.createIndex({"m.b": indexDirection, "m.i": indexDirection, t: indexDirection}));

    // Check that the rewrite is performed before caching.
    const pipeline = [{$sort: {"m.i": sortDirection, t: sortDirection}}, {$match: {"m.a": 1, "m.b": 1}}];
    let explain = coll.explain().aggregate(pipeline);
    assert.eq(getAggPlanStages(explain, stageName).length, 1, explain);
    const traversalDirection = sortDirection === indexDirection ? "forward" : "backward";
    assert.eq(getAggPlanStage(explain, "IXSCAN").direction, traversalDirection, explain);

    // Check the cache is empty.
    assert.eq(getTimeseriesCollForDDLOps(db, coll).getPlanCache().list().length, 0);

    // Run the query twice in order to cache and activate the plan.
    for (let i = 0; i < 2; ++i) {
        const result = coll.aggregate(pipeline).toArray();
        assert.eq(result.length, 20, result);
    }

    // Check the answer was cached.
    assert.eq(getTimeseriesCollForDDLOps(db, coll).getPlanCache().list().length, 1);

    // Check that the solution still uses internal bounded sort with the correct order.
    explain = coll.explain().aggregate(pipeline);
    assert.eq(getAggPlanStages(explain, stageName).length, 1, explain);
    assert.eq(getAggPlanStage(explain, "IXSCAN").direction, traversalDirection, explain);

    // Get constants needed for replanning.
    const cursorStageName = "$cursor";
    const planCacheKey = getPlanCacheKeyFromExplain(getAggPlanStage(explain, cursorStageName)[cursorStageName]);
    const planCacheEntry = (() => {
        const planCache = getTimeseriesCollForDDLOps(db, coll)
            .getPlanCache()
            .list([{$match: {planCacheKey}}]);
        assert.eq(planCache.length, 1, planCache);
        return planCache[0];
    })();
    let ratio = (() => {
        const getParamRes = assert.commandWorked(
            db.adminCommand({getParameter: 1, internalQueryCacheEvictionRatio: 1}),
        );
        return getParamRes["internalQueryCacheEvictionRatio"];
    })();

    // Remove existing docs, add docs to trigger replanning.
    assert.commandWorked(coll.deleteMany({"m.a": 1, "m.b": 1}));
    let numNewDocs = ratio * planCacheEntry.works + 1;
    addDocs(coll, numNewDocs, [1, 0]);
    addDocs(coll, numNewDocs, [0, 1]);

    // Turn on profiling.
    // Don't profile the setFCV command, which could be run during this test in the
    // fcv_upgrade_downgrade_replica_sets_jscore_passthrough suite.
    assert.commandWorked(
        db.setProfilingLevel(1, {filter: {"command.setFeatureCompatibilityVersion": {"$exists": false}}}),
    );

    // Rerun command with replanning.
    const comment = jsTestName();
    let result = coll.aggregate(pipeline, {comment}).toArray();
    assert.eq(result.length, 0);

    // Check that the plan was replanned.
    const replanProfileEntry = getLatestProfilerEntry(db, {"command.comment": comment});
    assert(replanProfileEntry.replanned, replanProfileEntry);

    // Check that rewrite happens with replanning.
    explain = coll.explain().aggregate(pipeline);
    assert.eq(getAggPlanStages(explain, stageName).length, 1, {explain, stages: getAggPlanStages(explain, stageName)});
    assert.eq(getAggPlanStage(explain, "IXSCAN").direction, traversalDirection, explain);
};

for (const sortDirection of [-1, 1]) {
    for (const indexDirection of [-1, 1]) {
        testBoundedSorterPlanCache(sortDirection, indexDirection);
    }
}
