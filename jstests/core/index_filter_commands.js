/**
 * Index Filter commands
 *
 * Commands:
 * - planCacheListFilters
 *   Displays index filters for all query shapes in a collection.
 *
 * - planCacheClearFilters
 *   Clears index filter for a single query shape or, if the query shape is omitted, all filters for
 *   the collection.
 *
 * - planCacheSetFilter
 *   Sets index filter for a query shape. Overrides existing filter.
 *
 * Not a lot of data access in this test suite. Hint commands manage a non-persistent mapping in the
 * server of query shape to list of index specs.
 *
 * Only time we might need to execute a query is to check the plan cache state. We would do this
 * using the $planCacheStats aggregation metadata source on the same query shape with the index
 * filters.
 *
 * @tags: [
 *   # Cannot implicitly shard accessed collections because of collection existing when none
 *   # expected.
 *   assumes_no_implicit_collection_creation_after_drop,
 *   # This test attempts to perform queries with plan cache filters set up. The former operation
 *   # may be routed to a secondary in the replica set, whereas the latter must be routed to the
 *   # primary.
 *   assumes_read_concern_unchanged,
 *   assumes_read_preference_unchanged,
 *   assumes_unsharded_collection,
 *   does_not_support_stepdowns,
 *   requires_fcv_60
 * ]
 */

(function() {
load("jstests/libs/analyze_plan.js");
load("jstests/libs/clustered_collections/clustered_collection_util.js");
load("jstests/libs/fixture_helpers.js");  // For 'FixtureHelpers'.
load("jstests/libs/sbe_explain_helpers.js");

const coll = db.jstests_index_filter_commands;

coll.drop();

// Setup the data so that plans will not tie given the indices and query
// below. Tying plans will not be cached, and we need cached shapes in
// order to test the filter functionality.
assert.commandWorked(coll.insert({a: 1}));
assert.commandWorked(coll.insert({a: 1}));
assert.commandWorked(coll.insert({a: 1, b: 1}));
assert.commandWorked(coll.insert({_id: 1}));

// Add 2 indexes.
// 1st index is more efficient.
// 2nd and 3rd indexes will be used to test index filters.
var indexA1 = {a: 1};
var indexA1B1 = {a: 1, b: 1};
var indexA1C1 = {a: 1, c: 1};
assert.commandWorked(coll.createIndex(indexA1));
assert.commandWorked(coll.createIndex(indexA1B1));
assert.commandWorked(coll.createIndex(indexA1C1));

var queryAA = {a: "A"};
var queryA1 = {a: 1, b: 1};
var projectionA1 = {_id: 0, a: 1};
var sortA1 = {a: -1};
var queryID = {_id: 1};

//
// Tests for planCacheListFilters, planCacheClearFilters, planCacheSetFilter
//

// Utility function to list index filters.
function getFilters(collection) {
    if (collection == undefined) {
        collection = coll;
    }
    var res = collection.runCommand('planCacheListFilters');
    assert.commandWorked(res, 'planCacheListFilters failed');
    assert(res.hasOwnProperty('filters'), 'filters missing from planCacheListFilters result');
    return res.filters;
}

// Utility function to clear index filters set on the 'collection'. The 'queryShape', if provided,
// is used to ensure that plans with the given shape have been removed from the cache.
function clearFilters(collection, queryShape, isPipeline = false) {
    if (collection == undefined) {
        collection = coll;
    }

    // Clear the filters set earlier.
    assert.commandWorked(collection.runCommand('planCacheClearFilters'));
    filters = getFilters(collection);
    assert.eq(
        0, filters.length, 'filters not cleared after successful planCacheClearFilters command');

    // Plans should be removed after clearing filters.
    if (queryShape) {
        const planCacheEntryFunc = isPipeline ? planCacheEntryForPipeline : planCacheEntryForQuery;
        assert.eq(
            null, planCacheEntryFunc(queryShape, collection), collection.getPlanCache().list());
    }
}

// Returns the plan cache entry for the given value of 'createdFromQuery', or null if no such plan
// cache entry exists.
function planCacheEntryForQuery(createdFromQuery, collection) {
    if (collection === undefined) {
        collection = coll;
    }

    const options = Object.assign({collection, db}, createdFromQuery);
    const planCacheKey = getPlanCacheKeyFromShape(options);
    const res = collection.getPlanCache().list([{$match: {planCacheKey: planCacheKey}}]);
    if (res.length === 0) {
        return null;
    }

    assert.eq(1, res.length, res);
    return res[0];
}

// Returns the plan cache entry for the given pipeline, or null if no such plan cache entry exists.
function planCacheEntryForPipeline(pipeline, collection) {
    if (collection === undefined) {
        collection = coll;
    }

    const planCacheKey = getPlanCacheKeyFromPipeline(pipeline, collection, db);
    const res = collection.getPlanCache().list([{$match: {planCacheKey: planCacheKey}}]);
    if (res.length === 0) {
        return null;
    }

    assert.eq(1, res.length, res);
    return res[0];
}

// Utility function to list plans for a query.
// Attempting to retrieve index filters on a non-existent collection
// will return empty results.
var missingCollection = db.jstests_index_filter_commands_missing;
missingCollection.drop();
assert.eq(0,
          getFilters(missingCollection),
          'planCacheListFilters should return empty array on non-existent collection');

// Retrieve index filters from an empty test collection.
var filters = getFilters();
assert.eq(0, filters.length, 'unexpected number of index filters in planCacheListFilters result');

// Check details of winning plan in plan cache before setting index filter.
assert.eq(1, coll.find(queryA1, projectionA1).sort(sortA1).itcount(), 'unexpected document count');
var shape = {query: queryA1, sort: sortA1, projection: projectionA1};
var planBeforeSetFilter = planCacheEntryForQuery(shape);
assert.neq(null, planBeforeSetFilter, coll.getPlanCache().list());
// Check 'indexFilterSet' field in plan details
assert.eq(false, planBeforeSetFilter.indexFilterSet, planBeforeSetFilter);

// Adding index filters to a non-existent collection should be an error.
assert.commandFailed(missingCollection.runCommand(
    'planCacheSetFilter',
    {query: queryA1, sort: sortA1, projection: projectionA1, indexes: [indexA1B1, indexA1C1]}));

// Add index filters for simple query.
assert.commandWorked(coll.runCommand(
    'planCacheSetFilter',
    {query: queryA1, sort: sortA1, projection: projectionA1, indexes: [indexA1B1, indexA1C1]}));
filters = getFilters();
assert.eq(
    1, filters.length, 'no change in query settings after successfully setting index filters');
assert.eq(queryA1, filters[0].query, 'unexpected query in filters');
assert.eq(sortA1, filters[0].sort, 'unexpected sort in filters');
assert.eq(projectionA1, filters[0].projection, 'unexpected projection in filters');
assert.eq(2, filters[0].indexes.length, 'unexpected number of indexes in filters');
assert.eq(indexA1B1, filters[0].indexes[0], 'unexpected first index');
assert.eq(indexA1C1, filters[0].indexes[1], 'unexpected first index');

// Plans for query shape should be removed after setting index filter.
assert.eq(null, planCacheEntryForQuery(shape), coll.getPlanCache().list());

// Check details of winning plan in plan cache after setting filter and re-executing query.
assert.eq(1, coll.find(queryA1, projectionA1).sort(sortA1).itcount(), 'unexpected document count');
planAfterSetFilter = planCacheEntryForQuery(shape);
assert.neq(null, planAfterSetFilter, coll.getPlanCache().list());
// Check 'indexFilterSet' field in plan details
assert.eq(true, planAfterSetFilter.indexFilterSet, planAfterSetFilter);

// Execute query with cursor.hint(). Check that user-provided hint is overridden.
// Applying the index filters will remove the user requested index from the list
// of indexes provided to the planner.
// If the planner still tries to use the user hint, we will get a 'bad hint' error.
coll.find(queryA1, projectionA1).sort(sortA1).hint(indexA1).itcount();

// Test that index filters are ignored for idhack queries.
assert.commandWorked(coll.runCommand('planCacheSetFilter', {query: queryID, indexes: [indexA1]}));
var explain = coll.explain("executionStats").find(queryID).finish();
assert.commandWorked(explain);

const winningPlan = getWinningPlan(explain.queryPlanner);
const collectionIsClustered = ClusteredCollectionUtil.areAllCollectionsClustered(db.getMongo());
if (collectionIsClustered) {
    assert(isClusteredIxscan(db, getWinningPlan(explain.queryPlanner)),
           "Expected clustered ixscan: " + tojson(explain));
} else {
    engineSpecificAssertion(
        isIdhack(db, winningPlan), isIdIndexScan(db, winningPlan, "FETCH"), db, winningPlan);
}

// Clearing filters on a missing collection should be a no-op.
assert.commandWorked(missingCollection.runCommand('planCacheClearFilters'));

print('Plan details before setting filter = ' + tojson(planBeforeSetFilter.details, '', true));
print('Plan details after setting filter = ' + tojson(planAfterSetFilter.details, '', true));

//
// Tests for the 'indexFilterSet' explain field.
//
if (!FixtureHelpers.isMongos(db)) {
    ['queryPlanner', 'executionStats', 'allPlansExecution'].forEach((verbosity) => {
        // Make sure to clean index filters before we run the test for each verbosity level.
        clearFilters(coll, shape);

        // No filter.
        coll.getPlanCache().clear();
        assert.eq(
            false, coll.find({z: 1}).explain(verbosity).queryPlanner.indexFilterSet, verbosity);
        assert.eq(false,
                  coll.find(queryA1, projectionA1)
                      .sort(sortA1)
                      .explain(verbosity)
                      .queryPlanner.indexFilterSet,
                  verbosity);

        // With one filter set.
        assert.commandWorked(
            coll.runCommand('planCacheSetFilter', {query: {z: 1}, indexes: [{z: 1}]}));
        assert.eq(
            true, coll.find({z: 1}).explain(verbosity).queryPlanner.indexFilterSet, verbosity);
        assert.eq(false,
                  coll.find(queryA1, projectionA1)
                      .sort(sortA1)
                      .explain(verbosity)
                      .queryPlanner.indexFilterSet,
                  verbosity);

        // With two filters set.
        assert.commandWorked(coll.runCommand('planCacheSetFilter', {
            query: queryA1,
            projection: projectionA1,
            sort: sortA1,
            indexes: [indexA1B1, indexA1C1]
        }));
        assert.eq(
            true, coll.find({z: 1}).explain(verbosity).queryPlanner.indexFilterSet, verbosity);
        assert.eq(true,
                  coll.find(queryA1, projectionA1)
                      .sort(sortA1)
                      .explain(verbosity)
                      .queryPlanner.indexFilterSet,
                  verbosity);
    });
} else {
    clearFilters(coll, shape);
}

//
// Tests for index filter commands and multiple indexes with the same key pattern.
//

assert(coll.drop());

var collationEN = {locale: "en_US"};
assert.commandWorked(coll.createIndex(indexA1, {collation: collationEN, name: "a_1:en_US"}));
assert.commandWorked(coll.createIndex(indexA1, {name: "a_1"}));

assert.commandWorked(coll.insert({a: "a"}));

assert.commandWorked(coll.runCommand('planCacheSetFilter', {query: queryAA, indexes: [indexA1]}));

assert.commandWorked(coll.runCommand('planCacheSetFilter',
                                     {query: queryAA, collation: collationEN, indexes: [indexA1]}));

// Ensure that index key patterns in planCacheSetFilter select any index with a matching key
// pattern.

explain = coll.find(queryAA).explain();
assert(isIxscan(db, getWinningPlan(explain.queryPlanner)),
       "Expected index scan: " + tojson(explain));

explain = coll.find(queryAA).collation(collationEN).explain();
assert(isIxscan(db, getWinningPlan(explain.queryPlanner)),
       "Expected index scan: " + tojson(explain));

// Ensure that index names in planCacheSetFilter only select matching names.

assert.commandWorked(coll.runCommand('planCacheSetFilter',
                                     {query: queryAA, collation: collationEN, indexes: ["a_1"]}));

explain = coll.find(queryAA).collation(collationEN).explain();
assert(isCollscan(db, getWinningPlan(explain.queryPlanner)),
       "Expected collscan: " + tojson(explain));

//
// Test that planCacheSetFilter and planCacheClearFilters allow queries containing $expr.
//

assert(coll.drop());
assert.commandWorked(coll.insert({a: "a"}));
assert.commandWorked(coll.createIndex(indexA1, {name: "a_1"}));

assert.commandWorked(coll.runCommand(
    "planCacheSetFilter", {query: {a: "a", $expr: {$eq: ["$a", "a"]}}, indexes: [indexA1]}));
filters = getFilters();
assert.eq(1, filters.length, tojson(filters));
assert.eq({a: "a", $expr: {$eq: ["$a", "a"]}}, filters[0].query, tojson(filters[0]));

assert.commandWorked(
    coll.runCommand("planCacheClearFilters", {query: {a: "a", $expr: {$eq: ["$a", "a"]}}}));
filters = getFilters();
assert.eq(0, filters.length, tojson(filters));

//
// Test that planCacheSetFilter and planCacheClearFilters do not allow queries containing $expr with
// unbound variables.
//

assert(coll.drop());
assert.commandWorked(coll.insert({a: "a"}));
assert.commandWorked(coll.createIndex(indexA1, {name: "a_1"}));

assert.commandFailed(
    coll.runCommand("planCacheSetFilter",
                    {query: {a: "a", $expr: {$eq: ["$a", "$$unbound"]}}, indexes: [indexA1]}));
filters = getFilters();
assert.eq(0, filters.length, tojson(filters));

assert.commandFailed(
    coll.runCommand("planCacheClearFilters", {query: {a: "a", $expr: {$eq: ["$a", "$$unbound"]}}}));
filters = getFilters();
assert.eq(0, filters.length, tojson(filters));

if (checkSBEEnabled(db, ["featureFlagSbeFull"])) {
    //
    // Test that planCacheSetFilter doesn't apply to the inner side of a $lookup.
    //
    assert(coll.drop());
    assert.commandWorked(coll.insert({a: 1, b: 1}));

    // Create a foreign collection with a set of indexes.
    const foreignColl = db.index_filter_commands_foreign;
    foreignColl.drop();
    assert.commandWorked(foreignColl.insert({a: 1, b: 1}));
    assert.commandWorked(foreignColl.createIndex(indexA1, {name: "foreign_a_1"}));
    assert.commandWorked(foreignColl.createIndex(indexA1B1, {name: "foreign_a_1_b_1"}));
    assert.commandWorked(foreignColl.createIndex(indexA1C1, {name: "foreign_a_1_c_1"}));

    // Add an index filter on the foreign collection that would hypothetically affect the selection
    // of an INLJ plan.
    assert.commandWorked(
        foreignColl.runCommand("planCacheSetFilter", {query: queryA1, indexes: [indexA1C1]}));
    filters = getFilters(foreignColl);
    assert.eq(1, filters.length, filters);
    assert.eq(queryA1, filters[0].query, filters);
    assert.eq(1, filters[0].indexes.length, filters);
    assert.eq(indexA1C1, filters[0].indexes[0], filters);

    // Make sure no index filters have been applied to the main collection.
    filters = getFilters(coll);
    assert.eq(0, filters.length, filters);

    // Construct a $lookup pipeline in such a way that the join predicate on the inner side has the
    // same shape as the filter on the main collection: {a: <val>} and run it.
    const pipeline = [
        {$match: queryA1},
        {$lookup: {from: foreignColl.getName(), localField: "a", foreignField: "a", as: "matched"}}
    ];
    let results = coll.aggregate(pipeline).toArray();

    // Check details of the cached plan.
    assert.eq(1, results.length, results);
    planAfterSetFilter = planCacheEntryForPipeline(pipeline);
    assert.neq(null, planAfterSetFilter, coll.getPlanCache().list());
    // Check 'indexFilterSet' field in plan details - no index filters should be applied.
    assert.eq(false, planAfterSetFilter.indexFilterSet, planAfterSetFilter);

    // Ensure that despite an index filter being set on the foreign collection, we're still using
    // heuristics to select an INLJ plan. This can be proved by showing that the index being used is
    // the 'foreign_a_1' one, rather than 'foreign_a_1_c_1' specified in the index filter, as the
    // INLJ heuristics always prefer an index with a narrower key pattern.
    explain = coll.explain().aggregate(pipeline);
    assert.commandWorked(explain);

    let lookupStage = getPlanStage(explain, "EQ_LOOKUP");
    assert.neq(null, lookupStage, explain);
    assert.eq(lookupStage.strategy, "IndexedLoopJoin", explain);
    assert.eq(lookupStage.indexName, "foreign_a_1");

    // Now, add the same set of indexes to the main collection as defined on the foreign collection.
    assert.commandWorked(coll.createIndex(indexA1, {name: "main_a_1"}));
    assert.commandWorked(coll.createIndex(indexA1B1, {name: "main_a_1_b_1"}));
    assert.commandWorked(coll.createIndex(indexA1C1, {name: "main_a_1_c_1"}));

    // Add the same index filter on the main collection as defined on the foreign collection.
    assert.commandWorked(
        coll.runCommand("planCacheSetFilter", {query: queryA1, indexes: [indexA1C1]}));
    filters = getFilters(coll);
    assert.eq(1, filters.length, filters);
    assert.eq(queryA1, filters[0].query, filters);
    assert.eq(1, filters[0].indexes.length, filters);
    assert.eq(indexA1C1, filters[0].indexes[0], filters);

    // Make sure we still have one index filter defined on the foreign collection.
    assert.commandWorked(
        foreignColl.runCommand("planCacheSetFilter", {query: queryA1, indexes: [indexA1C1]}));
    filters = getFilters(foreignColl);
    assert.eq(1, filters.length, filters);

    // Re-run the pipeline.
    results = coll.aggregate(pipeline).toArray();

    // Check details of the cached plan.
    assert.eq(1, results.length, results);
    planAfterSetFilter = planCacheEntryForPipeline(pipeline);
    assert.neq(null, planAfterSetFilter, coll.getPlanCache().list());
    // Check 'indexFilterSet' field in plan details - an index filter should be applied.
    assert.eq(true, planAfterSetFilter.indexFilterSet, planAfterSetFilter);

    // Check that the inner side was still using the heursitics to select an INLJ plan, and the
    // outer side honoured the index filter.
    explain = coll.explain().aggregate(pipeline);
    assert.commandWorked(explain);

    lookupStage = getPlanStage(explain, "EQ_LOOKUP");
    assert.neq(null, lookupStage, explain);
    assert.eq(lookupStage.strategy, "IndexedLoopJoin", explain);
    assert.eq(lookupStage.indexName, "foreign_a_1");

    ixscanStage = getPlanStage(explain, "IXSCAN");
    assert.neq(null, ixscanStage, explain);
    assert.eq(ixscanStage.indexName, "main_a_1_c_1", explain);

    //
    // Test that planCacheClearFilters only clears plan cache entries on the main collection.
    //

    // Clear the index filter on the foreign collection and check that the plan cache entry for the
    // $lookup still exists.
    assert.commandWorked(foreignColl.runCommand("planCacheClearFilters", {query: queryA1}));
    filters = getFilters(foreignColl);
    assert.eq(0, filters.length, filters);

    filters = getFilters(coll);
    assert.eq(1, filters.length, filters);
    assert.eq(queryA1, filters[0].query, filters);
    assert.eq(1, filters[0].indexes.length, filters);
    assert.eq(indexA1C1, filters[0].indexes[0], filters);

    let planCacheEntry = planCacheEntryForPipeline(pipeline);
    assert.neq(null, planCacheEntry, coll.getPlanCache().list());
    assert.eq(true, planCacheEntry.indexFilterSet, planCacheEntry);

    // Clear the index filter on the main collection and ensure that the plan is no longer in the
    // cache.
    assert.commandWorked(coll.runCommand("planCacheClearFilters", {query: queryA1}));
    filters = getFilters(coll);
    assert.eq(0, filters.length, filters);

    planCacheEntry = planCacheEntryForPipeline(pipeline);
    assert.eq(null, planCacheEntry, coll.getPlanCache().list());
}
}());
