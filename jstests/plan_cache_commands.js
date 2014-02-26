/**
 * Plan cache commands
 * 
 * Cache-wide Commands
 * - planCacheListQueryShapes
 * - planCacheClear
 *       Removes plans for one or all query shapes.
 * - planCacheListPlans
 */ 

var t = db.jstests_plan_cache_commands;
t.drop();

// Insert some data so we don't go to EOF.
t.save({a: 1, b: 1});
t.save({a: 2, b: 2});

// We need two indices so that the MultiPlanRunner is executed.
t.ensureIndex({a: 1});
t.ensureIndex({a: 1, b:1});

// Run the query.
var queryA1 = {a: 1, b:1};
var projectionA1 = {_id: 0, a: 1};
var sortA1 = {a: -1};
assert.eq(1, t.find(queryA1, projectionA1).sort(sortA1).itcount(), 'unexpected document count');
// We now expect the two indices to be compared and a cache entry to exist.


//
// tests for planCacheListQueryShapes
// Returns a list of query shapes for the queries currently cached in the collection.
//

// Utility function to list query shapes in cache.
function getShapes() {
    var res = t.runCommand('planCacheListQueryShapes');
    print('planCacheListQueryShapes() = ' + tojson(res));
    assert.commandWorked(res, 'planCacheListQueryShapes failed');
    assert(res.hasOwnProperty('shapes'), 'shapes missing from planCacheListQueryShapes result');
    return res.shapes;
    
}

// Attempting to retrieve cache information on non-existent collection is an error.
var missingCollection = db.jstests_query_cache_missing;
missingCollection.drop();
assert.commandFailed(missingCollection.runCommand('planCacheListQueryShapes'));

// Retrieve query shapes from the test collection
// Number of shapes should match queries executed by multi-plan runner.
var shapes = getShapes();
assert.eq(1, shapes.length, 'unexpected number of shapes in planCacheListQueryShapes result');
assert.eq({query: queryA1, sort: sortA1, projection: projectionA1}, shapes[0],
          'unexpected query shape returned from planCacheListQueryShapes');



//
// Tests for planCacheClear (one query shape)
//

// Invalid key should be an error.
assert.commandFailed(t.runCommand('planCacheClear', {query: {unknownfield: 1}}));

// Run a new query shape and drop it from the cache
assert.eq(1, t.find({a: 2, b: 2}).itcount(), 'unexpected document count');
assert.eq(2, getShapes().length, 'unexpected cache size after running 2nd query');
assert.commandWorked(t.runCommand('planCacheClear', {query: {a: 1, b: 1}}));
assert.eq(1, getShapes().length, 'unexpected cache size after dropping 2nd query from cache');



//
// Tests for planCacheListPlans
//

// Utility function to list plans for a query.
function getPlans(query, sort, projection) {
    var key = {query: query, sort: sort, projection: projection};
    var res = t.runCommand('planCacheListPlans', key);
    assert.commandWorked(res, 'planCacheListPlans(' + tojson(key, '', true) + ' failed');
    assert(res.hasOwnProperty('plans'), 'plans missing from planCacheListPlans(' +
           tojson(key, '', true) + ') result');
    return res.plans;
}

// Invalid key should be an error.
assert.commandFailed(t.runCommand('planCacheListPlans', {query: {unknownfield: 1}}));

// Retrieve plans for valid cache entry.
var plans = getPlans(queryA1, sortA1, projectionA1);
assert.eq(2, plans.length, 'unexpected number of plans cached for query');

// Print every plan
// Plan details/feedback verified separately in section after Query Plan Revision tests.
print('planCacheListPlans result:');
for (var i = 0; i < plans.length; i++) {
    print('plan ' + i + ': ' + tojson(plans[i]));
}



//
// Tests for planCacheClear
//

// Drop query cache. This clears all cached queries in the collection.
res = t.runCommand('planCacheClear');
print('planCacheClear() = ' + tojson(res));
assert.commandWorked(res, 'planCacheClear failed');
assert.eq(0, getShapes().length, 'plan cache should be empty after successful planCacheClear()');



//
// Query Plan Revision
// http://docs.mongodb.org/manual/core/query-plans/#query-plan-revision
// As collections change over time, the query optimizer deletes the query plan and re-evaluates
// after any of the following events:
// - The collection receives 1,000 write operations.
// - The reIndex rebuilds the index.
// - You add or drop an index.
// - The mongod process restarts.
//

// Case 1: The collection receives 1,000 write operations.
// Steps:
//     Populate cache. Cache should contain 1 key after running query.
//     Insert 1000 documents.
//     Cache should be cleared.
assert.eq(1, t.find(queryA1, projectionA1).sort(sortA1).itcount(), 'unexpected document count');
assert.eq(1, getShapes().length, 'plan cache should not be empty after query');
for (var i = 0; i < 1000; i++) {
    t.save({b: i});
}
assert.eq(0, getShapes().length, 'plan cache should be empty after adding 1000 documents.');

// Case 2: The reIndex rebuilds the index.
// Steps:
//     Populate the cache with 1 entry.
//     Run reIndex on the collection.
//     Confirm that cache is empty.
assert.eq(1, t.find(queryA1, projectionA1).sort(sortA1).itcount(), 'unexpected document count');
assert.eq(1, getShapes().length, 'plan cache should not be empty after query');
res = t.reIndex();
print('reIndex result = ' + tojson(res));
assert.eq(0, getShapes().length, 'plan cache should be empty after reIndex operation');

// Case 3: You add or drop an index.
// Steps:
//     Populate the cache with 1 entry.
//     Add an index.
//     Confirm that cache is empty.
assert.eq(1, t.find(queryA1, projectionA1).sort(sortA1).itcount(), 'unexpected document count');
assert.eq(1, getShapes().length, 'plan cache should not be empty after query');
t.ensureIndex({b: 1});
assert.eq(0, getShapes().length, 'plan cache should be empty after adding index');

// Case 4: The mongod process restarts
// Not applicable.



//
// Tests for plan reason and feedback in planCacheListPlans
//

// Generate more plans for test query by adding indexes (compound and sparse).
// This will also clear the plan cache.
t.ensureIndex({a: -1}, {sparse: true});
t.ensureIndex({a: 1, b: 1});

// Implementation note: feedback stats is calculated after 20 executions.
// See PlanCacheEntry::kMaxFeedback.
var numExecutions = 100;
for (var i = 0; i < numExecutions; i++) {
    assert.eq(1, t.find(queryA1, projectionA1).sort(sortA1).itcount(), 'query failed');
}

plans = getPlans(queryA1, sortA1, projectionA1);

// This should be obvious but feedback is available only for the first (winning) plan.
print('planCacheListPlans result (after adding indexes and completing 20 executions):');
for (var i = 0; i < plans.length; i++) {
    print('plan ' + i + ': ' + tojson(plans[i]));
    assert.gt(plans[i].reason.score, 0, 'plan ' + i + ' score is invalid');
    if (i > 0) {
        assert.lte(plans[i].reason.score, plans[i-1].reason.score,
                   'plans not sorted by score in descending order. ' +
                   'plan ' + i + ' has a score that is greater than that of the previous plan');
    }
    assert(plans[i].reason.stats.hasOwnProperty('type'), 'no stats inserted for plan ' + i);
}

// feedback meaningful only for plan 0
// feedback is capped at 20
assert.eq(20, plans[0].feedback.nfeedback, 'incorrect nfeedback');
assert.gt(plans[0].feedback.averageScore, 0, 'invalid average score');



//
// Tests for shell helpers
//

// Reset collection data and indexes.
t.drop();
var n = 200;
for (var i = 0; i < n; i++) {
    t.save({a:i, b: i});
}
t.ensureIndex({a: 1});
t.ensureIndex({b: 1});
t.ensureIndex({a: 1, b: 1});

// Repopulate plan cache with 3 query shapes.
var queryB = {a: {$gte: 0}, b: {$gte: 0}};
var projectionB = {_id: 0, b: 1};
var sortB = {b: -1};
assert.eq(n, t.find(queryB, projectionB).sort(sortB).itcount(), 'unexpected document count');
assert.eq(n, t.find(queryB, projectionB).itcount(), 'unexpected document count');
assert.eq(n, t.find(queryB).sort(sortB).itcount(), 'unexpected document count');
assert.eq(n, t.find(queryB).itcount(), 'unexpected document count');
assert.eq(4, getShapes().length, 'unexpected number of query shapes in plan cache');

//
// PlanCache.getName
//

var planCache = t.getPlanCache();
assert.eq(t.getName(), planCache.getName(), 'name of plan cache should match collection');

//
// PlanCache.help
//
planCache.help();

//
// shellPrint
//

print('plan cache:');
print(planCache);

//
// collection.getPlanCache().listQueryShapes
//

missingCollection.drop();
assert.throws(function() { missingCollection.getPlanCache().listQueryShapes() });
assert.eq(getShapes(), planCache.listQueryShapes(),
          'unexpected collection.getPlanCache().listQueryShapes() shell helper result');

//
// collection.getPlanCache().getPlansByQuery
//

// should error on non-existent collection.
assert.throws(function() { planCache.getPlansByQuery({unknownfield: 1}) });
// should error on missing required field query.
assert.throws(function() { planCache.getPlansByQuery() });

// Invoke with various permutations of required (query) and optional (projection, sort) arguments.
assert.eq(getPlans(queryB, sortB, projectionB), planCache.getPlansByQuery(queryB, projectionB,
                                                                          sortB),
          'plans from collection.getPlanCache().getPlansByQuery() different from command result');
assert.eq(getPlans(queryB, {}, projectionB), planCache.getPlansByQuery(queryB, projectionB),
          'plans from collection.getPlanCache().getPlansByQuery() different from command result');
assert.eq(getPlans(queryB, sortB, {}), planCache.getPlansByQuery(queryB, undefined, sortB),
          'plans from collection.getPlanCache().getPlansByQuery() different from command result');
assert.eq(getPlans(queryB, {}, {}), planCache.getPlansByQuery(queryB),
          'plans from collection.getPlanCache().getPlansByQuery() different from command result');

//
// collection.getPlanCache().clearPlansByQuery
//

// should error on non-existent collection.
assert.throws(function() { planCache.clearPlansByQuery({unknownfield: 1}) });
// should error on missing required field query.
assert.throws(function() { planCache.clearPlansByQuery() });

// Invoke with various permutations of required (query) and optional (projection, sort) arguments.
planCache.clearPlansByQuery(queryB, projectionB, sortB);
assert.eq(3, getShapes().length,
          'query shape not dropped after running collection.getPlanCache().clearPlansByQuery()');

planCache.clearPlansByQuery(queryB, projectionB);
assert.eq(2, getShapes().length,
          'query shape not dropped after running collection.getPlanCache().clearPlansByQuery()');

planCache.clearPlansByQuery(queryB, undefined, sortB);
assert.eq(1, getShapes().length,
          'query shape not dropped after running collection.getPlanCache().clearPlansByQuery()');

planCache.clearPlansByQuery(queryB);
assert.eq(0, getShapes().length,
          'query shape not dropped after running collection.getPlanCache().clearPlansByQuery()');


//
// collection.getPlanCache().clear
//

assert.throws(function() { missingCollection.getPlanCache().clear() });
// Re-populate plan cache with 1 query shape.
assert.eq(n, t.find(queryB, projectionB).sort(sortB).itcount(), 'unexpected document count');
assert.eq(1, getShapes().length, 'plan cache should not be empty after running cacheable query');
// Clear cache.
planCache.clear();
assert.eq(0, getShapes().length, 'plan cache not empty after clearing');



//
// explain for CachedPlanRunner
// This tests that the allPlans information in the explain output from CachedPlanRunner
// contains similar information to that of MultiPlanRunner.
//

planCache.clear();

// MultiPlanRunner explain
var multiPlanRunnerExplain = t.find(queryB, projectionB).sort(sortB).explain(true);

// CachedPlanRunner explain
var cachedPlanRunnerExplain = t.find(queryB, projectionB).sort(sortB).explain(true);

print('multi plan runner explain = ' + tojson(multiPlanRunnerExplain));
print('cached plan runner explain = ' + tojson(cachedPlanRunnerExplain));

assert.eq(multiPlanRunnerExplain.allPlans.length, cachedPlanRunnerExplain.allPlans.length,
          'explain for multi plan and cached plan runner should have same number of ' +
          'entries in allPlans');

for (var i = 0; i < multiPlanRunnerExplain.allPlans.length; ++i) {
    assert.neq(undefined, cachedPlanRunnerExplain.allPlans[i],
               'cound not find allPlans[' + i + '] in cached plan runner explain');
    assert.eq(multiPlanRunnerExplain.allPlans[i].cursor, cachedPlanRunnerExplain.allPlans[i].cursor,
              'explain for multi plan and cached plan runner should have same cursor for ' +
              'allPlans[' + i + ']');
}


//
// SERVER-12796: Plans for queries that return zero
// results should not be cached.
//

t.drop();

t.ensureIndex({a: 1});
t.ensureIndex({b: 1});

for (var i = 0; i < 200; i++) {
    t.save({a: 1, b: 1});
}
t.save({a: 2, b: 2});

// A query with zero results that does not hit EOF should not be cached...
assert.eq(0, t.find({c: 0}).itcount(), 'unexpected count');
assert.eq(0, getShapes().length, 'unexpected number of query shapes in plan cache');

// ...but a query with zero results that hits EOF will be cached.
assert.eq(1, t.find({a: 2, b: 2}).itcount(), 'unexpected count');
assert.eq(1, getShapes().length, 'unexpected number of query shapes in plan cache');

// A query that returns results but does not hit EOF will also be cached.
assert.eq(200, t.find({a: {$gte: 0}, b:1}).itcount(), 'unexpected count');
assert.eq(2, getShapes().length, 'unexpected number of query shapes in plan cache');
