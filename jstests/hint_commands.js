/**
 * Administrative hint commands
 * 
 * Commands:
 * - planCacheListHints
 *   Displays admin hints for all query shapes in a collection.
 *
 * - planCacheListClear
 *   Clears all admin hints for a single query shape or,
 *   if the query shape is omitted, for the entire collection.
 *
 * - planCacheSetHint
 *   Sets admin hints for a query shape. Overrides existing hints.
 *
 * Not a lot of data access in this test suite. Hint commands
 * manage a non-persistent mapping in the server of
 * query shape to list of index specs.
 *
 * Only time we might need to execute a query is to check the plan
 * cache state. We would do this with the planCacheListPlans command
 * on the same query shape with the admin hint.
 *
 */ 

var t = db.jstests_hint_commands;

t.drop();

t.save({a: 1});

// Add 2 indexes.
// 1st index is more efficient.
// 2nd and 3rd indexes will be used to test hint cache.
var indexA1 = {a: 1};
var indexA1B1 = {a: 1, b: 1};
var indexA1C1 = {a: 1, c: 1};
t.ensureIndex(indexA1);
t.ensureIndex(indexA1B1);
t.ensureIndex(indexA1C1);

var queryA1 = {a: 1};
var projectionA1 = {_id: 0, a: 1};
var sortA1 = {a: -1};

//
// Tests for planCacheListHints, planCacheClearHints, planCacheSetHint
//

// Utility function to list hints.
function getHints() {
    var res = t.runCommand('planCacheListHints');
    print('planCacheListHints() = ' + tojson(res));
    assert.commandWorked(res, 'planCacheListHints failed');
    assert(res.hasOwnProperty('hints'), 'hints missing from planCacheListHints result');
    return res.hints;
    
}

// Check if key is in plan cache.
function planCacheContains(shape) {
    var res = t.runCommand('planCacheListPlans', shape);
    return res.ok;
}

// Utility function to list plans for a query.
function getPlans(shape) {
    var res = t.runCommand('planCacheListPlans', shape);
    assert.commandWorked(res, 'planCacheListPlans(' + tojson(shape, '', true) + ' failed');
    assert(res.hasOwnProperty('plans'), 'plans missing from planCacheListPlans(' +
           tojson(shape, '', true) + ') result');
    return res.plans;
}

// It is an error to retrieve admin hints  on a non-existent collection.
var missingCollection = db.jstests_hint_commands_missing;
missingCollection.drop();
assert.commandFailed(missingCollection.runCommand('planCacheListHints'));

// Retrieve hints from an empty test collection.
var hints = getHints();
assert.eq(0, hints.length, 'unexpected number of hints in planCacheListHints result');

// Check details of winning plan in plan cache before setting hint.
assert.eq(1, t.find(queryA1, projectionA1).sort(sortA1).itcount(), 'unexpected document count');
var shape = {query: queryA1, sort: sortA1, projection: projectionA1};
var planBeforeSetHint = getPlans(shape)[0];
print('Winning plan (before setting admin hint) = ' + tojson(planBeforeSetHint));
// Check hint field in plan details
assert.eq(false, planBeforeSetHint.hint, 'missing or invalid hint field in plan details');

// Add hint for simple query
assert.commandWorked(t.runCommand('planCacheSetHint',
    {query: queryA1, sort: sortA1, projection: projectionA1, indexes: [indexA1B1, indexA1C1]}));
hints = getHints();
assert.eq(1, hints.length, 'no change in hint cache after successfully setting admin hint');
assert.eq(queryA1, hints[0].query, 'unexpected query in hints');
assert.eq(sortA1, hints[0].sort, 'unexpected sort in hints');
assert.eq(projectionA1, hints[0].projection, 'unexpected projection in hints');
assert.eq(2, hints[0].indexes.length, 'unexpected number of indexes in hints');
assert.eq(indexA1B1, hints[0].indexes[0], 'unexpected first index');
assert.eq(indexA1C1, hints[0].indexes[1], 'unexpected first index');

// Plans for query shape should be removed after setting hint.
assert(!planCacheContains(shape), 'plan cache for query shape not flushed after updating hint');

// Check details of winning plan in plan cache after setting hint and re-executing query.
assert.eq(1, t.find(queryA1, projectionA1).sort(sortA1).itcount(), 'unexpected document count');
planAfterSetHint = getPlans(shape)[0];
print('Winning plan (after setting admin hint) = ' + tojson(planAfterSetHint));
// Check hint field in plan details
assert.eq(true, planAfterSetHint.hint, 'missing or invalid hint field in plan details');

// Execute query with cursor.hint(). Check that user-provided hint is overridden.
// Applying the hint cache will remove the user requested index from the list
// of indexes provided to the planner.
// If the planner still tries to use the user hint, we will get a 'bad hint' error.
t.find(queryA1, projectionA1).sort(sortA1).hint(indexA1).itcount();

// Clear hints
assert.commandWorked(t.runCommand('planCacheClearHints'));
hints = getHints();
assert.eq(0, hints.length, 'hints not cleared after successful planCacheClearHints command');

// Plans should be removed after clearing hints
assert(!planCacheContains(shape), 'plan cache for query shape not flushed after clearing hints');

print('Plan details before setting hint = ' + tojson(planBeforeSetHint.details, '', true));
print('Plan details after setting hint = ' + tojson(planAfterSetHint.details, '', true));
