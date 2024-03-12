// Tests for using $planCacheStats to list cached plans.
//
// @tags: [
//   # If the balancer is on and chunks are moved, the plan cache can have entries with isActive:
//   # false when the test assumes they are true because the query has already been run many times.
//   assumes_balancer_off,
//   assumes_read_concern_unchanged,
//   # This test attempts to perform queries and introspect the server's plan cache entries. The
//   # former operation may be routed to a secondary in the replica set, whereas the latter must be
//   # routed to the primary.
//   assumes_read_preference_unchanged,
//   assumes_unsharded_collection,
//   does_not_support_stepdowns,
//   does_not_support_repeated_reads,
//   inspects_whether_plan_cache_entry_is_active,
//   requires_fcv_62,
//   # Plan cache state is node-local and will not get migrated alongside tenant data.
//   tenant_migration_incompatible,
//   # TODO SERVER-85728: Enable Bonsai plan cache tests involving indices.
//   cqf_experimental_incompatible,
//   references_foreign_collection,
//   # This tests perform queries and expect a particular number of candidate plans to be evaluated,
//   # creating unanticipated indexes can lead to a different number of candidate plans.
//   assumes_no_implicit_index_creation,
//   # Query settings are atlas proxy and direct shard execution incompatible.
//   directly_against_shardsvrs_incompatible,
//   simulate_atlas_proxy_incompatible,
//   # Query settings are not supported in upgrade/downgrade scenario
//   cannot_run_during_upgrade_downgrade,
//   # This test checks a new field "solutionHash" in $planCacheStats, not available in previous
//   # versions.
//   requires_fcv_72,
//   multiversion_incompatible,
// ]

import {
    getPlanCacheKeyFromPipeline,
    getPlanCacheKeyFromShape,
    getPlanStage
} from "jstests/libs/analyze_plan.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {QuerySettingsUtils} from "jstests/libs/query_settings_utils.js";
import {checkSbeFullyEnabled} from "jstests/libs/sbe_util.js";

let coll = db.jstests_plan_cache_list_plans;
coll.drop();

const isSbeEnabled = checkSbeFullyEnabled(db);

function dumpPlanCacheState() {
    return coll.aggregate([{$planCacheStats: {}}]).toArray();
}

function getPlansForCacheEntry(query = {}, sort = {}, projection = {}) {
    const keyHash = getPlanCacheKeyFromShape(
        {query: query, projection: projection, sort: sort, collection: coll, db: db});

    const res =
        coll.aggregate([{$planCacheStats: {}}, {$match: {planCacheKey: keyHash}}]).toArray();
    // We expect exactly one matching cache entry.
    assert.eq(1, res.length, dumpPlanCacheState());
    return res[0];
}

function getPlansForCacheEntryFromPipeline(pipeline) {
    const keyHash = getPlanCacheKeyFromPipeline(pipeline, coll, db);

    const res =
        coll.aggregate([{$planCacheStats: {}}, {$match: {planCacheKey: keyHash}}]).toArray();
    // We expect exactly one matching cache entry.
    assert.eq(1, res.length, dumpPlanCacheState());
    return res[0];
}

function assertNoCacheEntry(query, sort, projection) {
    const keyHash = getPlanCacheKeyFromShape(
        {query: query, projection: projection, sort: sort, collection: coll, db: db});

    assert.eq(0,
              coll.aggregate([{$planCacheStats: {}}, {$match: {planCacheKey: keyHash}}]).itcount(),
              dumpPlanCacheState());
}

// Assert that timeOfCreation exists in the cache entry. The difference between the current time
// and the time a plan was cached should not be larger than an hour.
function checkTimeOfCreation(query, sort, projection, date) {
    const keyHash = getPlanCacheKeyFromShape(
        {query: query, projection: projection, sort: sort, collection: coll, db: db});

    const res =
        coll.aggregate([{$planCacheStats: {}}, {$match: {planCacheKey: keyHash}}]).toArray();
    // We expect exactly one matching cache entry.
    assert.eq(1, res.length, res);
    const cacheEntry = res[0];

    assert(cacheEntry.hasOwnProperty('timeOfCreation'), cacheEntry);
    let kMillisecondsPerHour = 1000 * 60 * 60;
    assert.lte(
        Math.abs(date - cacheEntry.timeOfCreation.getTime()), kMillisecondsPerHour, cacheEntry);
}

assert.commandWorked(coll.insert({a: 1, b: 1}));
assert.commandWorked(coll.insert({a: 1, b: 2}));
assert.commandWorked(coll.insert({a: 1, b: 2}));
assert.commandWorked(coll.insert({a: 2, b: 2}));

// We need two indices so that the MultiPlanRunner is executed.
assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.createIndex({a: 1, b: 1}));

// Check that there are no cache entries associated with an unknown field.
assertNoCacheEntry({unknownfield: 1}, {}, {});

// Create a cache entry.
assert.eq(1,
          coll.find({a: 1, b: 1}, {_id: 0, a: 1}).sort({a: -1}).itcount(),
          'unexpected document count');

// Verify that the time of creation listed for the plan cache entry is reasonably close to 'now'.
let now = (new Date()).getTime();
checkTimeOfCreation({a: 1, b: 1}, {a: -1}, {_id: 0, a: 1}, now);

// Retrieve plans for valid cache entry.
let entry = getPlansForCacheEntry({a: 1, b: 1}, {a: -1}, {_id: 0, a: 1});
assert(entry.hasOwnProperty('works'), entry);
assert.eq(entry.isActive, false);

if (!isSbeEnabled) {
    // Note that SBE plan cache entry does not include "creationExecStats". We expect that there
    // were two candidate plans evaluated when the cache entry was created.
    assert(entry.hasOwnProperty("creationExecStats"), entry);
    assert.eq(2, entry.creationExecStats.length, entry);
}

// Test the queryHash and planCacheKey property by comparing entries for two different
// query shapes.
assert.eq(0, coll.find({a: 123}).sort({b: -1, a: 1}).itcount(), 'unexpected document count');
let entryNewShape = getPlansForCacheEntry({a: 123}, {b: -1, a: 1});
// Assert on queryHash.
assert.eq(entry.hasOwnProperty("queryHash"), true);
assert.eq(entryNewShape.hasOwnProperty("queryHash"), true);
assert.neq(entry["queryHash"], entryNewShape["queryHash"]);
// Assert on planCacheKey.
assert.eq(entry.hasOwnProperty("planCacheKey"), true);
assert.eq(entryNewShape.hasOwnProperty("planCacheKey"), true);
assert.neq(entry["planCacheKey"], entryNewShape["planCacheKey"]);
// Assert on solutionHash.
assert.eq(entry.hasOwnProperty("solutionHash"), true);
assert.eq(entryNewShape.hasOwnProperty("solutionHash"), true);
assert.neq(entry["solutionHash"], entryNewShape["solutionHash"]);

// Generate more plans for test query by adding indexes (compound and sparse).  This will also
// clear the plan cache.
assert.commandWorked(coll.createIndex({a: -1}, {sparse: true}));
assert.commandWorked(coll.createIndex({a: 1, b: 1}));

let numExecutions = 100;
for (let i = 0; i < numExecutions; i++) {
    assert.eq(0, coll.find({a: 3, b: 3}, {_id: 0, a: 1}).sort({a: -1}).itcount(), 'query failed');
}

// Verify that the time of creation listed for the plan cache entry is reasonably close to 'now'.
now = (new Date()).getTime();
checkTimeOfCreation({a: 3, b: 3}, {a: -1}, {_id: 0, a: 1}, now);

// Test that the cache entry is listed as active.
entry = getPlansForCacheEntry({a: 3, b: 3}, {a: -1}, {_id: 0, a: 1});
assert(entry.hasOwnProperty('works'), entry);
assert.eq(entry.isActive, true);

if (!isSbeEnabled) {
    // Note that SBE plan cache entry does not include "creationExecStats". There should be the same
    // number of candidate plan scores as candidate plans.
    assert.eq(entry.creationExecStats.length, entry.candidatePlanScores.length, entry);

    // Scores should be greater than zero and sorted descending.
    for (let i = 0; i < entry.candidatePlanScores.length; ++i) {
        const scores = entry.candidatePlanScores;
        assert.gt(scores[i], 0, entry);
        if (i > 0) {
            assert.lte(scores[i], scores[i - 1], entry);
        }
    }
} else {
    //
    // Test that $planCacheStats against a particular collection does not list cached $lookup plans
    // if the collection is the foreign collection (not the main collection).
    //
    const foreignColl = db.plan_cache_list_plans_foreign;
    foreignColl.drop();
    assert.commandWorked(foreignColl.insert({a: 1, b: 1}));
    assert.commandWorked(foreignColl.createIndex({b: 1}));

    const pipeline = [
        {$lookup: {from: foreignColl.getName(), localField: "a", foreignField: "b", as: "matched"}}
    ];
    const results = coll.aggregate(pipeline).toArray();
    assert.eq(4, results.length, results);

    // Make sure we have one plan cache entry for main collection and the plan is indexed NLJ.
    entry = getPlansForCacheEntryFromPipeline(pipeline);
    assert.eq(entry.isActive, true);

    const explain = coll.explain().aggregate(pipeline);
    assert.commandWorked(explain);

    if (!explain.splitPipeline) {
        const lookupStage = getPlanStage(explain, "EQ_LOOKUP");
        assert.neq(null, lookupStage, explain);
        assert.eq(lookupStage.strategy, "IndexedLoopJoin", explain);
        assert.eq(lookupStage.indexName, "b_1");

        // The '$planCacheStats' pipeline executed against the foreign collection shouldn't include
        // cached $lookup plans.
        const res = foreignColl.aggregate([{$planCacheStats: {}}]).toArray();
        assert.eq(0, res.length, dumpPlanCacheState());
    }
}

// Ensure query setting entry is present in $planCacheStats output.
// TODO: SERVER-71537 Remove Feature Flag for PM-412.
if (FeatureFlagUtil.isPresentAndEnabled(db, "QuerySettings") && !FixtureHelpers.isStandalone(db)) {
    // Set query settings for a query to use 'settings.indexHints.allowedIndexes' indexes.
    const qsutils = new QuerySettingsUtils(db, coll.getName());

    // Specify 'allowedIndexes' with more than one index, otherwise it will result in single
    // solution plan, that won't be cached in classic.
    const settings = {
        indexHints:
            {ns: {db: db.getName(), coll: coll.getName()}, allowedIndexes: ["a_1_b_1", "b_1_a_1"]}
    };
    assert.commandWorked(coll.createIndex({b: 1, a: 1}));
    const filter = {a: 1, b: 1};
    const query = qsutils.makeFindQueryInstance({filter});
    assert.commandWorked(db.adminCommand({setQuerySettings: query, settings: settings}));
    qsutils.assertQueryShapeConfiguration([qsutils.makeQueryShapeConfiguration(settings, query)]);

    // Run the query, such that a plan cache entry is created.
    assert.eq(1, coll.find(filter).itcount());

    // Ensure plan cache entry contains 'settings'.
    const planCacheEntry = getPlansForCacheEntry(filter);
    assert.eq(settings, planCacheEntry.querySettings, planCacheEntry);

    qsutils.removeAllQuerySettings();
}
