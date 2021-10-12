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
 * ]
 */

(function() {
load("jstests/libs/analyze_plan.js");
load("jstests/libs/fixture_helpers.js");  // For 'FixtureHelpers'.
load("jstests/libs/sbe_explain_helpers.js");
load("jstests/libs/sbe_util.js");  // For checkSBEEnabled.

if (checkSBEEnabled(db, ["featureFlagSbePlanCache"])) {
    jsTest.log("Skipping test because SBE and SBE plan cache are both enabled.");
    return;
}
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

// Returns the plan cache entry for the given value of 'createdFromQuery', or null if no such plan
// cache entry exists.
function planCacheEntryForQuery(createdFromQuery) {
    const res = coll.getPlanCache().list([{$match: {createdFromQuery: createdFromQuery}}]);
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
engineSpecificAssertion(
    isIdhack(db, winningPlan), isIdIndexScan(db, winningPlan, "FETCH"), db, winningPlan);
// Clear filters
// Clearing filters on a missing collection should be a no-op.
assert.commandWorked(missingCollection.runCommand('planCacheClearFilters'));
// Clear the filters set earlier.
assert.commandWorked(coll.runCommand('planCacheClearFilters'));
filters = getFilters();
assert.eq(0, filters.length, 'filters not cleared after successful planCacheClearFilters command');

// Plans should be removed after clearing filters.
assert.eq(null, planCacheEntryForQuery(shape), coll.getPlanCache().list());

print('Plan details before setting filter = ' + tojson(planBeforeSetFilter.details, '', true));
print('Plan details after setting filter = ' + tojson(planAfterSetFilter.details, '', true));

//
// Tests for the 'indexFilterSet' explain field.
//

if (!FixtureHelpers.isMongos(db)) {
    // No filter.
    coll.getPlanCache().clear();
    assert.eq(false, coll.find({z: 1}).explain('queryPlanner').queryPlanner.indexFilterSet);
    assert.eq(false,
              coll.find(queryA1, projectionA1)
                  .sort(sortA1)
                  .explain('queryPlanner')
                  .queryPlanner.indexFilterSet);

    // With one filter set.
    assert.commandWorked(coll.runCommand('planCacheSetFilter', {query: {z: 1}, indexes: [{z: 1}]}));
    assert.eq(true, coll.find({z: 1}).explain('queryPlanner').queryPlanner.indexFilterSet);
    assert.eq(false,
              coll.find(queryA1, projectionA1)
                  .sort(sortA1)
                  .explain('queryPlanner')
                  .queryPlanner.indexFilterSet);

    // With two filters set.
    assert.commandWorked(coll.runCommand(
        'planCacheSetFilter',
        {query: queryA1, projection: projectionA1, sort: sortA1, indexes: [indexA1B1, indexA1C1]}));
    assert.eq(true, coll.find({z: 1}).explain('queryPlanner').queryPlanner.indexFilterSet);
    assert.eq(true,
              coll.find(queryA1, projectionA1)
                  .sort(sortA1)
                  .explain('queryPlanner')
                  .queryPlanner.indexFilterSet);
}

//
// Tests for index filter commands and multiple indexes with the same key pattern.
//

coll.drop();

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

coll.drop();
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

coll.drop();
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
}());
