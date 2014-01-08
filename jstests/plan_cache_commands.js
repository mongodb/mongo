/**
 * Plan cache commands
 * 
 * Cache-wide Commands
 * - planCacheListKeys
 * - planCacheClear
 *
 * Query-specific Commands
 * - planCacheGenerateKey
 * - planCacheGet
 * - planCacheDrop
 * - planCacheListPlans
 * - planCachePinPlan
 * - planCacheUnpinPlan
 * - planCacheAddPlan
 * 
 * - planCacheShunPlan
 *       Prevents a plan from being used to execute the query.
 *       This plan will show up in planCacheListPlans results
 *       with shunned set to true. If a plan has been shunned,
 *       the only way to restore the plan is to drop the query
 *       from the cache using planCacheDrop and allow the query
 *       optimizer to re-populate the cache with plans for the
 *       query.
 */ 

var t = db.jstests_query_cache;

t.drop();

t.save({a: 1});

t.ensureIndex({a: 1});

var queryA1 = {a: 1};
var projectionA1 = {_id: 0, a: 1};
var sortA1 = {a: -1};
assert.eq(1, t.find(queryA1, projectionA1).sort(sortA1).itcount(), 'unexpected document count');



//
// Tests for planCacheGenerateKey
//

// Utility function to generate cache key.
function generateKey(cmdObj) {
    var res = t.runCommand('planCacheGenerateKey', cmdObj);
    print('planCacheGenerateKey(' + tojson(cmdObj, '', true) + ') = ' + tojson(res));
    assert.commandWorked(res, 'planCacheGenerateKey failed for ' + tojson(cmdObj, '', true));
    assert(res.hasOwnProperty('key'), 'key missing from planCacheGenerateKey(' +
           tojson(cmdObj, '', true) + ') result');
    assert.neq(null, res.key, 'null key returned by planCacheGenerateKey(' +
            tojson(cmdObj, '', true) + ')');
    return res.key;
}

// Invalid sort
assert.commandFailed(t.runCommand('planCacheGenerateKey', {query: {}, sort: {a: 'foo'}}));

// Valid query {a: 1} should return a non-empty cache key.
var keyA1 = generateKey({query: queryA1, sort: sortA1, projection: projectionA1});



//
// tests for planCacheListKeys
// Returns a list of keys for the queries currently cached in the collection.
//

// Utility function to list keys in cache.
function getKeys() {
    var res = t.runCommand('planCacheListKeys');
    print('planCacheListKeys() = ' + tojson(res));
    assert.commandWorked(res, 'planCacheListKeys failed');
    assert(res.hasOwnProperty('queries'), 'queries missing from planCacheListKeys result');
    return res.queries;
    
}
// Attempting to retrieve cache information on non-existent collection
// is an error.
var missingCollection = db.jstests_query_cache_missing;
missingCollection.drop();
assert.commandFailed(missingCollection.runCommand('planCacheListKeys'));

// Retrieve cache keys from the test collection
// Number of keys should match queries executed by multi-plan runner.
var keys = getKeys();
assert.eq(1, keys.length, 'unexpected number of keys in planCacheListKeys result');
assert.eq(keyA1, keys[0], 'unexpected cache key returned from planCacheListKeys');



//
// Tests for planCacheGet
//

// Invalid key should result in an error.
assert.commandFailed(t.runCommand('planCacheGet', {key: 'unknownquery'}));

// Get details on a query. This does not include plans.
var res = t.runCommand('planCacheGet', {key: keyA1});
print('planCacheGet({key: ' + tojson(keyA1) + ') = ' + tojson(res));
assert.commandWorked(res, 'planCacheGet failed');
assert.eq(queryA1, res.query, 'query in planCacheGetResult does not match initial query filter');
assert.eq(sortA1, res.sort, 'sort in planCacheGetResult does not match initial sort order');
assert.eq(projectionA1, res.projection, 'projection missing from planCacheGet result');



//
// Tests for planCacheDrop
//

// Invalid key should be an error.
assert.commandFailed(t.runCommand('planCacheDrop', {key: 'unknownquery'}));

// Run a new query shape and drop it from the cache
assert.eq(0, t.find({a: 1, b: 1}).itcount(), 'unexpected document count');
var keyA1B1 = generateKey({query: {a: 1, b: 1}, sort: {}, projection: {}});
assert.eq(2, getKeys().length, 'unexpected cache size after running 2nd query');
assert.commandWorked(t.runCommand('planCacheDrop', {key: keyA1B1}));
assert.eq(1, getKeys().length, 'unexpected cache size after dropping 2nd query from cache');



//
// Tests for planCacheListPlans
//

// Utility function to list plans for a query.
function getPlans(key) {
    var res = t.runCommand('planCacheListPlans', {key: key});
    assert.commandWorked(res, 'planCacheListPlans(' + tojson(key, '', true) + ' failed');
    assert(res.hasOwnProperty('plans'), 'plans missing from planCacheListPlans(' +
           tojson(key, '', true) + ') result');
    return res.plans;
}

// Invalid key should be an error.
assert.commandFailed(t.runCommand('planCacheListPlans', {key: 'unknownquery'}));

// Retrieve plans for valid cache entry.
var plans = getPlans(keyA1);
assert.eq(2, plans.length, 'unexpected number of plans cached for query');

// Print every plan
print('planCacheListPlans result:');
for (var i = 0; i < plans.length; i++) {
    print('plan ' + i + ': ' + tojson(plans[i]));
}


//
// Tests for planCachePinPlan
//

// Invalid key should be an error.
assert.commandFailed(t.runCommand('planCachePinPlan', {key: 'unknownquery', plan: 'plan1'}));

// Plan ID has to be provided.
assert.commandFailed(t.runCommand('planCachePinPlan', {key: keyA1}));

var plan1 = getPlans(keyA1)[1].plan;
res = t.runCommand('planCachePinPlan', {key: keyA1, plan: plan1});
assert.commandWorked(res, 'planCachePinPlan failed');

// Retrieve plans for valid cache entry after pinning
plans = getPlans(keyA1);
assert.eq(2, plans.length, 'unexpected number of plans cached for query after pinning');

// Print every plan
print('planCacheListPlans (after pinning) result:');
for (var i = 0; i < plans.length; i++) {
    print('plan ' + i + ': ' + tojson(plans[i]));
}
assert(plans[1].pinned, 'plan1 should be listed as pinned');



//
// Tests for planCacheUnpinPlan
//

// Invalid key should be an error.
assert.commandFailed(t.runCommand('planCacheUnpinPlan', {key: 'unknownquery'}));

res = t.runCommand('planCacheUnpinPlan', {key: keyA1});
assert.commandWorked(res, 'planCacheUnpinPlan failed');

// Retrieve plans for valid cache entry after unpinning
plans = getPlans(keyA1);
assert.eq(2, plans.length, 'unexpected number of plans cached for query after unpinning');

// Print every plan
print('planCacheListPlans (after unpinning) result:');
for (var i = 0; i < plans.length; i++) {
    print('plan ' + i + ': ' + tojson(plans[i]));
}
assert(!plans[1].pinned, 'plan1 should not be listed as pinned');



//
// Tests for planCacheAddPlan
//

// Invalid key should be an error.
assert.commandFailed(t.runCommand('planCacheAddPlan', {key: 'unknownquery', details: {}}));

// Plan details must to be provided.
assert.commandFailed(t.runCommand('planCacheAddPlan', {key: keyA1}));

// XXX: Adding a plan is not very meaningful at the moment. Adds non-executable plan
// to plan cache.
// Returns ID of added plan.
var numPlansBeforeAdd = getPlans(keyA1).length;
res = t.runCommand('planCacheAddPlan', {key: keyA1, details: {}});
assert.commandWorked(res, 'planCacheAddPlan failed');
print('planCacheAddPlan results = ' + tojson(res));
assert(res.hasOwnProperty('plan'), 'plan missing from planCacheAddPlan result');
plans = getPlans(keyA1);
var numPlansAfterAdd = plans.length;
assert.eq(numPlansBeforeAdd + 1, numPlansAfterAdd, 'number of plans cached unchanged');
var planAdded = res.plan;

// Print every plan
print('planCacheListPlans (after adding) result:');
for (var i = 0; i < plans.length; i++) {
    print('plan ' + i + ': ' + tojson(plans[i]));
}
var planAddedIndex = numPlansAfterAdd - 1;
assert.eq(planAdded, plans[planAddedIndex].plan,
          'added plan not found at end of planCacheListPlans result');



//
// Tests for planCacheShunPlan
//

// Invalid key should be an error.
assert.commandFailed(t.runCommand('planCacheShunPlan', {key: 'unknownquery', plan: planAdded}));

// Plan must to be provided.
assert.commandFailed(t.runCommand('planCacheShunPlan', {key: keyA1}));

// Invalid plan is not acceptable.
assert.commandFailed(t.runCommand('planCacheShunPlan', {key: keyA1, plan: 'bogusplan'}));

// Shunning plan should update shunned field in planCacheListPlans result.
var numPlansBeforeShun = getPlans(keyA1).length;
var plan0 = getPlans(keyA1)[0].plan;
res = t.runCommand('planCacheShunPlan', {key: keyA1, plan: plan0});
assert.commandWorked(res, 'planCacheShunPlan failed');
plans = getPlans(keyA1);
var numPlansAfterShun = plans.length;
assert.eq(numPlansBeforeShun, numPlansAfterShun, 'number of plans cached unchanged');

// Print every plan
print('planCacheListPlans (after shunning) result:');
for (var i = 0; i < plans.length; i++) {
    print('plan ' + i + ': ' + tojson(plans[i]));
}
assert(plans[0].shunned, 'first plan is not shown as shunned in planCacheListPlans result');



//
// Tests for planCacheClear
//

// Drop query cache. This clears all cached queries in the collection.
res = t.runCommand('planCacheClear');
print('planCacheClear() = ' + tojson(res));
assert.commandWorked(res, 'planCacheClear failed');
assert.eq(0, getKeys().length, 'plan cache should be empty after successful planCacheClear()');



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
assert.eq(1, getKeys().length, 'plan cache should not be empty after query');
for (var i = 0; i < 1000; i++) {
    t.save({b: i});
}
assert.eq(0, getKeys().length, 'plan cache should be empty after adding 1000 documents.');

// Case 2: The reIndex rebuilds the index.
// Steps:
//     Populate the cache with 1 entry.
//     Run reIndex on the collection.
//     Confirm that cache is empty.
assert.eq(1, t.find(queryA1, projectionA1).sort(sortA1).itcount(), 'unexpected document count');
assert.eq(1, getKeys().length, 'plan cache should not be empty after query');
res = t.reIndex();
print('reIndex result = ' + tojson(res));
assert.eq(0, getKeys().length, 'plan cache should be empty after reIndex operation');

// Case 3: You add or drop an index.
// Steps:
//     Populate the cache with 1 entry.
//     Add an index.
//     Confirm that cache is empty.
assert.eq(1, t.find(queryA1, projectionA1).sort(sortA1).itcount(), 'unexpected document count');
assert.eq(1, getKeys().length, 'plan cache should not be empty after query');
t.ensureIndex({b: 1});
assert.eq(0, getKeys().length, 'plan cache should be empty after adding index');

// Case 4: The mongod process restarts
// Not applicable.
