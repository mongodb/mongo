// Test the planCacheListPlans command.

var t = db.jstests_plan_cache_list_plans;
t.drop();

// Utility function to list plans for a query.
function getPlans(query, sort, projection) {
    var key = {query: query, sort: sort, projection: projection};
    var res = t.runCommand('planCacheListPlans', key);
    assert.commandWorked(res, 'planCacheListPlans(' + tojson(key, '', true) + ' failed');
    assert(res.hasOwnProperty('plans'),
           'plans missing from planCacheListPlans(' + tojson(key, '', true) + ') result');
    return res.plans;
}

t.save({a: 1, b: 1});
t.save({a: 1, b: 2});
t.save({a: 1, b: 2});
t.save({a: 2, b: 2});

// We need two indices so that the MultiPlanRunner is executed.
t.ensureIndex({a: 1});
t.ensureIndex({a: 1, b: 1});

// Invalid key should be an error.
assert.eq(0,
          getPlans({unknownfield: 1}, {}, {}),
          'planCacheListPlans should return empty results on unknown query shape');

// Create a cache entry.
assert.eq(
    1, t.find({a: 1, b: 1}, {_id: 0, a: 1}).sort({a: -1}).itcount(), 'unexpected document count');

// Retrieve plans for valid cache entry.
var plans = getPlans({a: 1, b: 1}, {a: -1}, {_id: 0, a: 1});
assert.eq(2, plans.length, 'unexpected number of plans cached for query');

// Print every plan
// Plan details/feedback verified separately in section after Query Plan Revision tests.
print('planCacheListPlans result:');
for (var i = 0; i < plans.length; i++) {
    print('plan ' + i + ': ' + tojson(plans[i]));
}

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
    assert.eq(0, t.find({a: 3, b: 3}, {_id: 0, a: 1}).sort({a: -1}).itcount(), 'query failed');
}

plans = getPlans({a: 3, b: 3}, {a: -1}, {_id: 0, a: 1});

// This should be obvious but feedback is available only for the first (winning) plan.
print('planCacheListPlans result (after adding indexes and completing 20 executions):');
for (var i = 0; i < plans.length; i++) {
    print('plan ' + i + ': ' + tojson(plans[i]));
    assert.gt(plans[i].reason.score, 0, 'plan ' + i + ' score is invalid');
    if (i > 0) {
        assert.lte(plans[i].reason.score,
                   plans[i - 1].reason.score,
                   'plans not sorted by score in descending order. ' +
                       'plan ' + i + ' has a score that is greater than that of the previous plan');
    }
    assert(plans[i].reason.stats.hasOwnProperty('stage'), 'no stats inserted for plan ' + i);
}
