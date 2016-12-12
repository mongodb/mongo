/**
 * Index Filter commands
 *
 * Commands:
 * - planCacheListFilters
 *   Displays index filters for all query shapes in a collection.
 *
 * - planCacheClearFilters
 *   Clears index filter for a single query shape or,
 *   if the query shape is omitted, all filters for the collection.
 *
 * - planCacheSetFilter
 *   Sets index filter for a query shape. Overrides existing filter.
 *
 * Not a lot of data access in this test suite. Hint commands
 * manage a non-persistent mapping in the server of
 * query shape to list of index specs.
 *
 * Only time we might need to execute a query is to check the plan
 * cache state. We would do this with the planCacheListPlans command
 * on the same query shape with the index filters.
 *
 */

load("jstests/libs/analyze_plan.js");

var t = db.jstests_index_filter_commands;

t.drop();

// Setup the data so that plans will not tie given the indices and query
// below. Tying plans will not be cached, and we need cached shapes in
// order to test the filter functionality.
t.save({a: 1});
t.save({a: 1});
t.save({a: 1, b: 1});
t.save({_id: 1});

// Add 2 indexes.
// 1st index is more efficient.
// 2nd and 3rd indexes will be used to test index filters.
var indexA1 = {a: 1};
var indexA1B1 = {a: 1, b: 1};
var indexA1C1 = {a: 1, c: 1};
t.ensureIndex(indexA1);
t.ensureIndex(indexA1B1);
t.ensureIndex(indexA1C1);

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
        collection = t;
    }
    var res = collection.runCommand('planCacheListFilters');
    print('planCacheListFilters() = ' + tojson(res));
    assert.commandWorked(res, 'planCacheListFilters failed');
    assert(res.hasOwnProperty('filters'), 'filters missing from planCacheListFilters result');
    return res.filters;
}

// If query shape is in plan cache,
// planCacheListPlans returns non-empty array of plans.
function planCacheContains(shape) {
    var res = t.runCommand('planCacheListPlans', shape);
    assert.commandWorked(res);
    return res.plans.length > 0;
}

// Utility function to list plans for a query.
function getPlans(shape) {
    var res = t.runCommand('planCacheListPlans', shape);
    assert.commandWorked(res, 'planCacheListPlans(' + tojson(shape, '', true) + ' failed');
    assert(res.hasOwnProperty('plans'),
           'plans missing from planCacheListPlans(' + tojson(shape, '', true) + ') result');
    return res.plans;
}

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
assert.eq(1, t.find(queryA1, projectionA1).sort(sortA1).itcount(), 'unexpected document count');
var shape = {query: queryA1, sort: sortA1, projection: projectionA1};
var planBeforeSetFilter = getPlans(shape)[0];
print('Winning plan (before setting index filters) = ' + tojson(planBeforeSetFilter));
// Check filterSet field in plan details
assert.eq(
    false, planBeforeSetFilter.filterSet, 'missing or invalid filterSet field in plan details');

// Adding index filters to a non-existent collection should be an error.
assert.commandFailed(missingCollection.runCommand(
    'planCacheSetFilter',
    {query: queryA1, sort: sortA1, projection: projectionA1, indexes: [indexA1B1, indexA1C1]}));

// Add index filters for simple query.
assert.commandWorked(t.runCommand(
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
assert(!planCacheContains(shape), 'plan cache for query shape not flushed after updating filter');

// Check details of winning plan in plan cache after setting filter and re-executing query.
assert.eq(1, t.find(queryA1, projectionA1).sort(sortA1).itcount(), 'unexpected document count');
planAfterSetFilter = getPlans(shape)[0];
print('Winning plan (after setting index filter) = ' + tojson(planAfterSetFilter));
// Check filterSet field in plan details
assert.eq(true, planAfterSetFilter.filterSet, 'missing or invalid filterSet field in plan details');

// Execute query with cursor.hint(). Check that user-provided hint is overridden.
// Applying the index filters will remove the user requested index from the list
// of indexes provided to the planner.
// If the planner still tries to use the user hint, we will get a 'bad hint' error.
t.find(queryA1, projectionA1).sort(sortA1).hint(indexA1).itcount();

// Test that index filters are ignored for idhack queries.
assert.commandWorked(t.runCommand('planCacheSetFilter', {query: queryID, indexes: [indexA1]}));
var explain = t.explain("executionStats").find(queryID).finish();
assert.commandWorked(explain);
var planStage = getPlanStage(explain.executionStats.executionStages, 'IDHACK');
assert.neq(null, planStage);

// Clear filters
// Clearing filters on a missing collection should be a no-op.
assert.commandWorked(missingCollection.runCommand('planCacheClearFilters'));
// Clear the filters set earlier.
assert.commandWorked(t.runCommand('planCacheClearFilters'));
filters = getFilters();
assert.eq(0, filters.length, 'filters not cleared after successful planCacheClearFilters command');

// Plans should be removed after clearing filters
assert(!planCacheContains(shape), 'plan cache for query shape not flushed after clearing filters');

print('Plan details before setting filter = ' + tojson(planBeforeSetFilter.details, '', true));
print('Plan details after setting filter = ' + tojson(planAfterSetFilter.details, '', true));

//
// Tests for the 'indexFilterSet' explain field.
//

if (db.isMaster().msg !== "isdbgrid") {
    // No filter.
    t.getPlanCache().clear();
    assert.eq(false, t.find({z: 1}).explain('queryPlanner').queryPlanner.indexFilterSet);
    assert.eq(false,
              t.find(queryA1, projectionA1)
                  .sort(sortA1)
                  .explain('queryPlanner')
                  .queryPlanner.indexFilterSet);

    // With one filter set.
    assert.commandWorked(t.runCommand('planCacheSetFilter', {query: {z: 1}, indexes: [{z: 1}]}));
    assert.eq(true, t.find({z: 1}).explain('queryPlanner').queryPlanner.indexFilterSet);
    assert.eq(false,
              t.find(queryA1, projectionA1)
                  .sort(sortA1)
                  .explain('queryPlanner')
                  .queryPlanner.indexFilterSet);

    // With two filters set.
    assert.commandWorked(t.runCommand(
        'planCacheSetFilter',
        {query: queryA1, projection: projectionA1, sort: sortA1, indexes: [indexA1B1, indexA1C1]}));
    assert.eq(true, t.find({z: 1}).explain('queryPlanner').queryPlanner.indexFilterSet);
    assert.eq(true,
              t.find(queryA1, projectionA1)
                  .sort(sortA1)
                  .explain('queryPlanner')
                  .queryPlanner.indexFilterSet);
}

//
// Tests for index filter commands and multiple indexes with the same key pattern.
//

t.drop();

var collationEN = {locale: "en_US"};
assert.commandWorked(t.createIndex(indexA1, {collation: collationEN, name: "a_1:en_US"}));
assert.commandWorked(t.createIndex(indexA1, {name: "a_1"}));

assert.writeOK(t.insert({a: "a"}));

assert.commandWorked(t.runCommand('planCacheSetFilter', {query: queryAA, indexes: [indexA1]}));

assert.commandWorked(t.runCommand('planCacheSetFilter',
                                  {query: queryAA, collation: collationEN, indexes: [indexA1]}));

// Ensure that index key patterns in planCacheSetFilter select any index with a matching key
// pattern.

explain = t.find(queryAA).explain();
assert(isIxscan(explain.queryPlanner.winningPlan), "Expected index scan: " + tojson(explain));

explain = t.find(queryAA).collation(collationEN).explain();
assert(isIxscan(explain.queryPlanner.winningPlan), "Expected index scan: " + tojson(explain));

// Ensure that index names in planCacheSetFilter only select matching names.

assert.commandWorked(
    t.runCommand('planCacheSetFilter', {query: queryAA, collation: collationEN, indexes: ["a_1"]}));

explain = t.find(queryAA).collation(collationEN).explain();
assert(isCollscan(explain.queryPlanner.winningPlan), "Expected collscan: " + tojson(explain));
