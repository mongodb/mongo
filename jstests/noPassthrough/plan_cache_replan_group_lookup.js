/**
 * Test that plans with $group and $lookup lowered to SBE are cached and replanned as appropriate.
 * @tags: [
 *   requires_profiling,
 * ]
 */
import {getAggPlanStages, getEngine, getPlanStage} from "jstests/libs/analyze_plan.js";
import {assertCacheUsage, setUpActiveCacheEntry} from "jstests/libs/plan_cache_utils.js";
import {getLatestProfilerEntry} from "jstests/libs/profiler.js";
import {
    checkSbeFullFeatureFlagEnabled,
    checkSbeRestrictedOrFullyEnabled,
} from "jstests/libs/sbe_util.js";

const conn = MongoRunner.runMongod();
const db = conn.getDB("test");
const coll = db.plan_cache_replan_group_lookup;
const foreignCollName = "foreign";
coll.drop();

const sbeEnabled = checkSbeRestrictedOrFullyEnabled(db);
const sbePlanCacheEnabled = checkSbeFullFeatureFlagEnabled(db);
assert.commandWorked(db.setProfilingLevel(2));

// Carefully construct a collection so that some queries will do well with an {a: 1} index
// and others with a {b: 1} index.
for (let i = 1000; i < 1100; i++) {
    assert.commandWorked(coll.insert({a: 1, b: i, c: 5}));
    assert.commandWorked(coll.insert({a: 1, b: i, c: 6}));
    assert.commandWorked(coll.insert({a: 1, b: i, c: 7}));
}
for (let i = 1000; i < 1100; i++) {
    assert.commandWorked(coll.insert({a: i, b: 1, c: 5}));
    assert.commandWorked(coll.insert({a: i, b: 1, c: 8}));
    assert.commandWorked(coll.insert({a: i, b: 1, c: 8}));
}
assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.createIndex({b: 1}));

function assertN2(cursor) {
    const res = cursor.toArray();
    assert.eq(2, res[0].n, res);
}

function testFn(aIndexPipeline,
                bIndexPipeline,
                cacheEntryVersion,
                setUpFn = undefined,
                tearDownFn = undefined,
                explainFn = undefined) {
    if (setUpFn) {
        setUpFn();
    }

    if (explainFn) {
        explainFn(aIndexPipeline);
        explainFn(bIndexPipeline);
    }

    setUpActiveCacheEntry(
        coll, aIndexPipeline, cacheEntryVersion, "a_1" /* cachedIndexName */, assertN2);

    // Now run the other pipeline, which has the same query shape but is faster with a different
    // index. It should trigger re-planning of the query.
    assert.eq(3, coll.aggregate(bIndexPipeline).toArray()[0].n);

    // The other pipeline again, The cache should be used now.
    assertCacheUsage({
        queryColl: coll,
        pipeline: bIndexPipeline,
        fromMultiPlanning: true,
        cacheEntryVersion: cacheEntryVersion,
        cacheEntryIsActive: true,
        cachedIndexName: "b_1"
    });

    // Run it once again so that the cache entry is reused.
    assert.eq(3, coll.aggregate(bIndexPipeline).toArray()[0].n);

    assertCacheUsage({
        queryColl: coll,
        pipeline: bIndexPipeline,
        fromMultiPlanning: false,
        cacheEntryVersion: cacheEntryVersion,
        cacheEntryIsActive: true,
        cachedIndexName: "b_1"
    });

    if (tearDownFn) {
        tearDownFn();
    }

    coll.getPlanCache().clear();
}

// This pipeline will be quick with {a: 1} index, and far slower {b: 1} index. With the {a: 1}
// index, the server should only need to examine one document. Using {b: 1}, it will have to
// scan through each document which has 2 as the value of the 'b' field.
const aIndexPredicate = [{$match: {a: 1042, b: 1}}];

// Opposite of 'aIndexQuery'. Should be quick if the {b: 1} index is used, and slower if the
// {a: 1} index is used.
const bIndexPredicate = [{$match: {a: 1, b: 1042}}];

const expectedVersion = sbePlanCacheEnabled ? 2 : 1;
// $group tests.
const groupSuffix = [{$group: {_id: "$c"}}, {$count: "n"}];
testFn(aIndexPredicate.concat(groupSuffix),
       bIndexPredicate.concat(groupSuffix),
       expectedVersion /* cacheEntryVersion */);

// $lookup tests.
const lookupStage =
    [{$lookup: {from: foreignCollName, localField: 'c', foreignField: 'foreignKey', as: 'out'}}];
const aLookup = aIndexPredicate.concat(lookupStage).concat(groupSuffix);
const bLookup = bIndexPredicate.concat(lookupStage).concat(groupSuffix);

function createLookupForeignColl() {
    const foreignColl = db[foreignCollName];

    // Here, the values for 'foreignKey' are expected to match existing values for 'c' in 'coll'.
    assert.commandWorked(foreignColl.insert([{foreignKey: 8}, {foreignKey: 6}]));
}

function dropLookupForeignColl() {
    assert(db[foreignCollName].drop());
}

function verifyCorrectLookupAlgorithmUsed(targetJoinAlgorithm, pipeline, aggOptions = {}) {
    if (!sbeEnabled) {
        return;
    }

    const explain = coll.explain().aggregate(pipeline, aggOptions);
    const eqLookupNodes = getAggPlanStages(explain, "EQ_LOOKUP");

    // Verify via explain that $lookup was lowered and appropriate $lookup algorithm was chosen.
    assert.eq(
        eqLookupNodes.length, 1, "expected at least one EQ_LOOKUP node; got " + tojson(explain));
    assert.eq(eqLookupNodes[0].strategy, targetJoinAlgorithm);
}

// NLJ.
testFn(aLookup,
       bLookup,
       expectedVersion /* cacheEntryVersion */,
       createLookupForeignColl,
       dropLookupForeignColl,
       (pipeline) =>
           verifyCorrectLookupAlgorithmUsed("NestedLoopJoin", pipeline, {allowDiskUse: false}));

// INLJ.
testFn(aLookup,
       bLookup,
       expectedVersion /* cacheEntryVersion */,
       () => {
           createLookupForeignColl();
           assert.commandWorked(db[foreignCollName].createIndex({foreignKey: 1}));
       },
       dropLookupForeignColl,
       (pipeline) =>
           verifyCorrectLookupAlgorithmUsed("IndexedLoopJoin", pipeline, {allowDiskUse: false}));

// HJ.
testFn(aLookup, bLookup, expectedVersion /* cacheEntryVersion */, () => {
    createLookupForeignColl();
}, dropLookupForeignColl, (pipeline) => verifyCorrectLookupAlgorithmUsed("HashJoin", pipeline, {
                              allowDiskUse: true
                          }));

// Verify that a cached plan which initially uses an INLJ will use HJ once the index is dropped and
// the foreign collection is dropped, and NLJ when 'allowDiskUse' is set to 'false'.

// For the first run, the query should go through multiplanning and create inactive cache entry.
createLookupForeignColl();
assert.commandWorked(db[foreignCollName].createIndex({foreignKey: 1}));
verifyCorrectLookupAlgorithmUsed("IndexedLoopJoin", aLookup, {allowDiskUse: true});
setUpActiveCacheEntry(
    coll, aLookup, expectedVersion /* cacheEntryVersion */, "a_1" /* cachedIndexName */, assertN2);

// Drop the index. This should result in using HJ.
assert.commandWorked(db[foreignCollName].dropIndex({foreignKey: 1}));
verifyCorrectLookupAlgorithmUsed("HashJoin", aLookup, {allowDiskUse: true});
assert.eq(2, coll.aggregate(aLookup).toArray()[0].n);
// If SBE plan cache is enabled, a new cache entry will be created in the SBE plan cache after
// invalidation. The corresponding cache entry in SBE plan cache should be inactive because the SBE
// plan cache is invalidated on index drop.
assertCacheUsage({
    queryColl: coll,
    pipeline: aLookup,
    fromMultiPlanning: sbePlanCacheEnabled,
    cacheEntryVersion: expectedVersion,
    cacheEntryIsActive: !sbePlanCacheEnabled,
    cachedIndexName: "a_1"
});

// Set 'allowDiskUse' to 'false'. This should still result in using NLJ.
verifyCorrectLookupAlgorithmUsed("NestedLoopJoin", aLookup, {allowDiskUse: false});
assert.eq(2, coll.aggregate(aLookup).toArray()[0].n);
// Note that multi-planning is expected here when the SBE plan cache is enabled because the
// 'allowDiskUse' value is part of the SBE plan cache key encoding.
assertCacheUsage({
    queryColl: coll,
    pipeline: aLookup,
    fromMultiPlanning: sbePlanCacheEnabled,
    cacheEntryVersion: expectedVersion,
    cacheEntryIsActive: true,
    cachedIndexName: "a_1"
});

// Drop the foreign collection.
dropLookupForeignColl();
verifyCorrectLookupAlgorithmUsed("NonExistentForeignCollection", aLookup, {allowDiskUse: true});
assert.eq(2, coll.aggregate(aLookup).toArray()[0].n);
assertCacheUsage({
    queryColl: coll,
    pipeline: aLookup,
    fromMultiPlanning: sbePlanCacheEnabled,
    cacheEntryVersion: expectedVersion,
    cacheEntryIsActive: !sbePlanCacheEnabled,
    cachedIndexName: "a_1"
});

// Verify that changing the plan for the right side does not trigger a replan.
const foreignColl = db[foreignCollName];
coll.drop();
foreignColl.drop();
coll.getPlanCache().clear();

assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.createIndex({b: 1}));
assert.commandWorked(coll.insert([
    {_id: 0, a: 1, b: 1},
    {_id: 1, a: 1, b: 2},
    {_id: 2, a: 1, b: 3},
    {_id: 3, a: 1, b: 4},
]));

assert.commandWorked(foreignColl.createIndex({c: 1}));
for (let i = -30; i < 30; ++i) {
    assert.commandWorked(foreignColl.insert({_id: i, c: i}));
}

const avoidReplanLookupPipeline = [
    {$match: {a: 1, b: 3}},
    {$lookup: {from: foreignColl.getName(), as: "as", localField: "a", foreignField: "c"}}
];
function runLookupQuery(options = {}) {
    assert.eq([{_id: 2, a: 1, b: 3, as: [{_id: 1, c: 1}]}],
              coll.aggregate(avoidReplanLookupPipeline, options).toArray());
}

// Verify that we are using IndexedLoopJoin.
verifyCorrectLookupAlgorithmUsed(
    "IndexedLoopJoin", avoidReplanLookupPipeline, {allowDiskUse: false});

runLookupQuery({allowDiskUse: false});
assertCacheUsage({
    queryColl: coll,
    pipeline: avoidReplanLookupPipeline,
    fromMultiPlanning: true,
    cacheEntryVersion: expectedVersion,
    cacheEntryIsActive: false,
    cachedIndexName: "b_1",
    aggOptions: {allowDiskUse: false}
});

runLookupQuery({allowDiskUse: false});
assertCacheUsage({
    queryColl: coll,
    pipeline: avoidReplanLookupPipeline,
    fromMultiPlanning: true,
    cacheEntryVersion: expectedVersion,
    cacheEntryIsActive: true,
    cachedIndexName: "b_1",
    aggOptions: {allowDiskUse: false}
});

// After dropping the index on the right-hand side, we should NOT replan the cached query. We
// will, however, choose a different join algorithm.
assert.commandWorked(foreignColl.dropIndex({c: 1}));

// Verify that we are now using NestedLoopJoin.
verifyCorrectLookupAlgorithmUsed(
    "NestedLoopJoin", avoidReplanLookupPipeline, {allowDiskUse: false});

// If SBE plan cache is enabled, after dropping index, the $lookup plan cache will be invalidated.
// We will need to rerun the multi-planner.
if (sbePlanCacheEnabled) {
    runLookupQuery({allowDiskUse: false});
    assertCacheUsage({
        queryColl: coll,
        pipeline: avoidReplanLookupPipeline,
        fromMultiPlanning: true,
        cacheEntryVersion: 2,
        cacheEntryIsActive: false,
        cachedIndexName: "b_1",
        aggOptions: {allowDiskUse: false}
    });

    runLookupQuery({allowDiskUse: false});
    assertCacheUsage({
        queryColl: coll,
        pipeline: avoidReplanLookupPipeline,
        fromMultiPlanning: true,
        cacheEntryVersion: 2,
        cacheEntryIsActive: true,
        cachedIndexName: "b_1",
        aggOptions: {allowDiskUse: false}
    });
}

runLookupQuery({allowDiskUse: false});
assertCacheUsage({
    queryColl: coll,
    pipeline: avoidReplanLookupPipeline,
    fromMultiPlanning: false,
    cacheEntryVersion: expectedVersion,
    cacheEntryIsActive: true,
    cachedIndexName: "b_1",
    aggOptions: {allowDiskUse: false}
});

runLookupQuery({allowDiskUse: false});
assertCacheUsage({
    queryColl: coll,
    pipeline: avoidReplanLookupPipeline,
    fromMultiPlanning: false,
    cacheEntryVersion: expectedVersion,
    cacheEntryIsActive: true,
    cachedIndexName: "b_1",
    aggOptions: {allowDiskUse: false}
});

// Run with 'allowDiskUse: true'. This should now use HashJoin, and we should still avoid
// replanning the cached query.
verifyCorrectLookupAlgorithmUsed("HashJoin", avoidReplanLookupPipeline, {allowDiskUse: true});

// If SBE plan cache is enabled, using different 'allowDiskUse' option will result in
// different plan cache key.
if (sbePlanCacheEnabled) {
    runLookupQuery({allowDiskUse: true});
    assertCacheUsage({
        queryColl: coll,
        pipeline: avoidReplanLookupPipeline,
        fromMultiPlanning: true,
        cacheEntryVersion: 2,
        cacheEntryIsActive: false,
        cachedIndexName: "b_1",
        aggOptions: {allowDiskUse: true}
    });

    runLookupQuery({allowDiskUse: true});
    assertCacheUsage({
        queryColl: coll,
        pipeline: avoidReplanLookupPipeline,
        fromMultiPlanning: true,
        cacheEntryVersion: 2,
        cacheEntryIsActive: true,
        cachedIndexName: "b_1",
        aggOptions: {allowDiskUse: true}
    });
}

runLookupQuery({allowDiskUse: true});
assertCacheUsage({
    queryColl: coll,
    pipeline: avoidReplanLookupPipeline,
    fromMultiPlanning: false,
    cacheEntryVersion: expectedVersion,
    cacheEntryIsActive: true,
    cachedIndexName: "b_1",
    aggOptions: {allowDiskUse: true}
});
runLookupQuery({allowDiskUse: true});
assertCacheUsage({
    queryColl: coll,
    pipeline: avoidReplanLookupPipeline,
    fromMultiPlanning: false,
    cacheEntryVersion: expectedVersion,
    cacheEntryIsActive: true,
    cachedIndexName: "b_1",
    aggOptions: {allowDiskUse: true}
});

// Verify that disabling $lookup pushdown into SBE does not trigger a replan, and uses the
// correct engine to execute results.
coll.getPlanCache().clear();
assert.commandWorked(foreignColl.createIndex({c: 1}));

const avoidReplanGroupPipeline = [
    {$match: {a: 1, b: 3}},
    {$group: {_id: "$a", out: {"$sum": 1}}},
];

function runGroupQuery() {
    assert.eq([{_id: 1, out: 1}], coll.aggregate(avoidReplanGroupPipeline).toArray());
}

// Verify that we are using IndexedLoopJoin.
verifyCorrectLookupAlgorithmUsed("IndexedLoopJoin", avoidReplanLookupPipeline);

// Set up an active cache entry.
runLookupQuery();
assertCacheUsage({
    queryColl: coll,
    pipeline: avoidReplanLookupPipeline,
    fromMultiPlanning: true,
    cacheEntryVersion: expectedVersion,
    cacheEntryIsActive: false,
    cachedIndexName: "b_1"
});

runLookupQuery();
assertCacheUsage({
    queryColl: coll,
    pipeline: avoidReplanLookupPipeline,
    fromMultiPlanning: true,
    cacheEntryVersion: expectedVersion,
    cacheEntryIsActive: true,
    cachedIndexName: "b_1"
});
runLookupQuery();
assertCacheUsage({
    queryColl: coll,
    pipeline: avoidReplanLookupPipeline,
    fromMultiPlanning: false,
    cacheEntryVersion: expectedVersion,
    cacheEntryIsActive: true,
    cachedIndexName: "b_1"
});
runLookupQuery();
assertCacheUsage({
    queryColl: coll,
    pipeline: avoidReplanLookupPipeline,
    fromMultiPlanning: false,
    cacheEntryVersion: expectedVersion,
    cacheEntryIsActive: true,
    cachedIndexName: "b_1"
});

/**
 * Tests if replanning and cache invalidation are performed for hash-join plans, when foreign
 * collection size increases.
 *
 * 'singleSolution' indicates whether the initial aggregation pipeline run will result in a single
 * solution.
 */
function testReplanningAndCacheInvalidationOnForeignCollSizeIncrease(singleSolution) {
    if (!sbeEnabled) {
        return;
    }

    const coll = db.plan_cache_replan_group_lookup_coll_resize;
    const foreignColl = db.plan_cache_replan_group_lookup_coll_resize_foreign;

    const pipeline = [
        {$match: {a: 2, b: 2}},
        {$lookup: {from: foreignColl.getName(), localField: "a", foreignField: "a", as: "out"}}
    ];

    function runLookup() {
        assert.eq([{_id: 2, a: 2, b: 2, out: [{_id: 1, a: 2, b: 1}]}],
                  coll.aggregate(pipeline).toArray());
    }

    // Asserts that the plan cache has only one entry and checks if it has a hash_lookup stage.
    function assertPlanCacheEntry({shouldHaveHashLookup}) {
        const entries = coll.getPlanCache().list();
        assert.eq(entries.length, 1, entries);
        assert(entries[0].isActive, entries[0]);

        let hasHashLookup = false;
        if (sbePlanCacheEnabled) {
            assert.eq(entries[0].version, "2", entries[0]);
            if (singleSolution) {
                // Single solution plans do not have a 'worksType' field.
                assert(!("worksType" in entries[0]));
            } else {
                assert.eq(entries[0].worksType, "reads", entries[0]);
            }
            hasHashLookup = entries[0].cachedPlan.stages.includes("hash_lookup");
        } else {
            assert.eq(entries[0].version, "1", entries[0]);
            assert.eq(entries[0].worksType, sbeEnabled ? "reads" : "works");
            // As a sanity check, we look for EQ_LOOKUP in the cached plan. The classic cache
            // should never contain nodes from pipeline stages, so we should never expect to find
            // it.
            hasHashLookup = getPlanStage(entries[0].cachedPlan, "EQ_LOOKUP") != null;
        }
        assert.eq(shouldHaveHashLookup, hasHashLookup, entries[0]);
    }

    // Set maximum number of documents in the foreign collection to 5.
    const initialMaxNoOfDocuments =
        assert
            .commandWorked(db.adminCommand(
                {getParameter: 1, internalQueryCollectionMaxNoOfDocumentsToChooseHashJoin: 1}))
            .internalQueryCollectionMaxNoOfDocumentsToChooseHashJoin;
    assert.commandWorked(db.adminCommand(
        {setParameter: 1, internalQueryCollectionMaxNoOfDocumentsToChooseHashJoin: 5}));

    coll.drop();
    foreignColl.drop();

    if (!singleSolution) {
        assert.commandWorked(coll.createIndex({a: 1}));
        assert.commandWorked(coll.createIndex({b: 1}));
    }

    assert.commandWorked(coll.insert({_id: 1, a: 1, b: 1}));
    assert.commandWorked(coll.insert({_id: 2, a: 2, b: 2}));
    assert.commandWorked(coll.insert({_id: 3, a: 2, b: 3}));
    assert.commandWorked(coll.insert({_id: 4, a: 3, b: 4}));
    assert.commandWorked(coll.insert({_id: 5, a: 5, b: 5}));
    assert.commandWorked(coll.insert({_id: 6, a: 6, b: 6}));
    assert.commandWorked(coll.insert({_id: 7, a: 6, b: 7}));
    assert.commandWorked(foreignColl.insert({_id: 1, a: 2, b: 1}));
    assert.commandWorked(foreignColl.insert({_id: 2, a: 3, b: 2}));

    // Ensure that plan cache entry has a plan with hash-join in it.
    runLookup();
    runLookup();

    // TODO SERVER-90880: Check whether this assertion should be updated.
    if (sbePlanCacheEnabled || !singleSolution) {
        // We should have a HashLookup in the cache only when the SBE cache is enabled. Otherwise,
        // we're only caching the outer side of the plan.
        assertPlanCacheEntry({shouldHaveHashLookup: sbePlanCacheEnabled});
    }
    verifyCorrectLookupAlgorithmUsed("HashJoin", pipeline);

    // Increase the size of the foreign collection
    assert.commandWorked(foreignColl.insert({a: 3, b: 3}));
    assert.commandWorked(foreignColl.insert({a: 5, b: 4}));
    assert.commandWorked(foreignColl.insert({a: 6, b: 5}));
    assert.commandWorked(foreignColl.insert({a: 6, b: 6}));
    assert.commandWorked(foreignColl.insert({a: 7, b: 7}));

    // Ensure that plan cache entry does not have a plan with hash-join in it.
    runLookup();
    runLookup();

    // TODO SERVER-90880: Check whether this assertion should be updated.
    if (sbePlanCacheEnabled || !singleSolution) {
        // Regardless of whether SBE plan cache is enabled, we should have a plan that does not have
        // a HashLookup in the cache.
        assertPlanCacheEntry({shouldHaveHashLookup: false});
    }

    // Ensure that hash-join is no longer used in the plan.
    verifyCorrectLookupAlgorithmUsed("NestedLoopJoin", pipeline);

    // Reset the 'internalQueryCollectionMaxNoOfDocumentsToChooseHashJoin' knob.
    assert.commandWorked(db.adminCommand({
        setParameter: 1,
        internalQueryCollectionMaxNoOfDocumentsToChooseHashJoin: initialMaxNoOfDocuments
    }));
}
testReplanningAndCacheInvalidationOnForeignCollSizeIncrease(true /* singleSolution */);
testReplanningAndCacheInvalidationOnForeignCollSizeIncrease(false /* singleSolution */);

{
    // Now we check whether the cache entry used for the $lookup run above can be re-used after
    // disabling lookup pushdown.

    // Check whether the $lookup uses SBE.
    let explain = coll.explain().aggregate(avoidReplanLookupPipeline);
    const lookupUsedSbeByDefault = getAggPlanStages(explain, "EQ_LOOKUP").length > 0;

    // Disable $lookup pushdown. This should not invalidate the cache entry, but it should prevent
    // $lookup from being pushed down.
    assert.commandWorked(db.adminCommand(
        {setParameter: 1, internalQuerySlotBasedExecutionDisableLookupPushdown: true}));

    // Verify via explain that $lookup was NOT lowered.
    explain = coll.explain().aggregate(avoidReplanLookupPipeline);
    const eqLookupNodes = getAggPlanStages(explain, "EQ_LOOKUP");
    assert.eq(eqLookupNodes.length, 0, "expected no EQ_LOOKUP nodes; got " + tojson(explain));
    let engineAfterDisableLookupPushdown = getEngine(explain);

    if (sbePlanCacheEnabled) {
        runLookupQuery();
        const profileObj = getLatestProfilerEntry(db, {op: "command", ns: coll.getFullName()});
        const matchingCacheEntries = coll.getPlanCache().list(
            [{$match: {planCacheShapeHash: profileObj.planCacheShapeHash}}]);
        assert.eq(1, matchingCacheEntries.length);
    } else if (lookupUsedSbeByDefault && engineAfterDisableLookupPushdown == "classic") {
        // If the lookup used SBE by default, then we cannot re-use the cache entry when we re-run
        // it in classic.
        runLookupQuery();
        assertCacheUsage({
            queryColl: coll,
            pipeline: avoidReplanLookupPipeline,
            fromMultiPlanning: true,
            cacheEntryVersion: 1,
            cacheEntryIsActive: false,
            cachedIndexName: "b_1"
        });
        runLookupQuery();
        assertCacheUsage({
            queryColl: coll,
            pipeline: avoidReplanLookupPipeline,
            fromMultiPlanning: true,
            cacheEntryVersion: 1,
            cacheEntryIsActive: true,
            cachedIndexName: "b_1"
        });
        runLookupQuery();
        assertCacheUsage({
            queryColl: coll,
            pipeline: avoidReplanLookupPipeline,
            fromMultiPlanning: false,
            cacheEntryVersion: 1,
            cacheEntryIsActive: true,
            cachedIndexName: "b_1"
        });
    } else {
        // Otherwise, we did not use SBE for the $lookup pipeline by default. In this case we can
        // re-use the cache entry that was created earlier.
        runLookupQuery();
        assertCacheUsage({
            queryColl: coll,
            pipeline: avoidReplanLookupPipeline,
            fromMultiPlanning: false,
            cacheEntryVersion: 1,
            cacheEntryIsActive: true,
            cachedIndexName: "b_1"
        });
        runLookupQuery();
        assertCacheUsage({
            queryColl: coll,
            pipeline: avoidReplanLookupPipeline,
            fromMultiPlanning: false,
            cacheEntryVersion: 1,
            cacheEntryIsActive: true,
            cachedIndexName: "b_1"
        });
    }
}

// Verify that disabling $group pushdown into SBE does not trigger a replan, and uses the
// correct engine to execute results.
coll.getPlanCache().clear();

// Verify that $group gets pushed down, provided that SBE is enabled.
let groupNodes;
let explain = null;
if (sbeEnabled) {
    explain = coll.explain().aggregate(avoidReplanGroupPipeline);
    let groupNodes = getAggPlanStages(explain, "GROUP");
    assert.eq(groupNodes.length, 1);
}

// Set up an active cache entry.
runGroupQuery();
assertCacheUsage({
    queryColl: coll,
    pipeline: avoidReplanGroupPipeline,
    fromMultiPlanning: true,
    cacheEntryVersion: expectedVersion,
    cacheEntryIsActive: false,
    cachedIndexName: "b_1"
});
runGroupQuery();
assertCacheUsage({
    queryColl: coll,
    pipeline: avoidReplanGroupPipeline,
    fromMultiPlanning: true,
    cacheEntryVersion: expectedVersion,
    cacheEntryIsActive: true,
    cachedIndexName: "b_1"
});

runGroupQuery();
assertCacheUsage({
    queryColl: coll,
    pipeline: avoidReplanGroupPipeline,
    fromMultiPlanning: false,
    cacheEntryVersion: expectedVersion,
    cacheEntryIsActive: true,
    cachedIndexName: "b_1"
});
runGroupQuery();
assertCacheUsage({
    queryColl: coll,
    pipeline: avoidReplanGroupPipeline,
    fromMultiPlanning: false,
    cacheEntryVersion: expectedVersion,
    cacheEntryIsActive: true,
    cachedIndexName: "b_1"
});

explain = coll.explain().aggregate(avoidReplanGroupPipeline);
const groupUsedSbeByDefault = getEngine(explain) === "sbe";

// Disable $group pushdown. This should not invalidate the cache entry, but it should prevent $group
// from being pushed down.
assert.commandWorked(
    db.adminCommand({setParameter: 1, internalQuerySlotBasedExecutionDisableGroupPushdown: true}));

explain = coll.explain().aggregate(avoidReplanGroupPipeline);
groupNodes = getAggPlanStages(explain, "GROUP");
assert.eq(groupNodes.length, 0);
const engineUsedAfterGroupPushdownDisabled = getEngine(explain);

if (sbePlanCacheEnabled) {
    runGroupQuery();
    const profileObj = getLatestProfilerEntry(db, {op: "command", ns: coll.getFullName()});
    const matchingCacheEntries =
        coll.getPlanCache().list([{$match: {planCacheShapeHash: profileObj.planCacheShapeHash}}]);
    assert.eq(1, matchingCacheEntries.length);
} else if (groupUsedSbeByDefault && engineUsedAfterGroupPushdownDisabled === "classic") {
    // In other cases, we will NOT be able to re-use the same cache entry, since the group query
    // ran in SBE, and now that group pushdown is disabled, it will be marked as SBE incompatible.
    runGroupQuery();
    assertCacheUsage({
        queryColl: coll,
        pipeline: avoidReplanGroupPipeline,
        fromMultiPlanning: true,
        cacheEntryVersion: 1,
        cacheEntryIsActive: false,
        cachedIndexName: "b_1"
    });
    runGroupQuery();
    assertCacheUsage({
        queryColl: coll,
        pipeline: avoidReplanGroupPipeline,
        fromMultiPlanning: true,
        cacheEntryVersion: 1,
        cacheEntryIsActive: true,
        cachedIndexName: "b_1"
    });

    runGroupQuery();
    assertCacheUsage({
        queryColl: coll,
        pipeline: avoidReplanGroupPipeline,
        fromMultiPlanning: false,
        cacheEntryVersion: 1,
        cacheEntryIsActive: true,
        cachedIndexName: "b_1"
    });
} else {
    // When the SBE is completely disabled, we will be able to reuse the same cache entry.
    runGroupQuery();
    assertCacheUsage({
        queryColl: coll,
        pipeline: avoidReplanGroupPipeline,
        fromMultiPlanning: false,
        cacheEntryVersion: 1,
        cacheEntryIsActive: true,
        cachedIndexName: "b_1"
    });
    runGroupQuery();
    assertCacheUsage({
        queryColl: coll,
        pipeline: avoidReplanGroupPipeline,
        fromMultiPlanning: false,
        cacheEntryVersion: 1,
        cacheEntryIsActive: true,
        cachedIndexName: "b_1"
    });
}

MongoRunner.stopMongod(conn);
