// Test the planCacheListPlans command.
//
// @tags: [
//   # This test attempts to perform queries and introspect the server's plan cache entries. The
//   # former operation may be routed to a secondary in the replica set, whereas the latter must be
//   # routed to the primary.
//   # If the balancer is on and chunks are moved, the plan cache can have entries with isActive:
//   # false when the test assumes they are true because the query has already been run many times.
//   assumes_read_preference_unchanged,
//   does_not_support_stepdowns,
//   assumes_balancer_off,
// ]

(function() {
    "use strict";
    let t = db.jstests_plan_cache_list_plans;
    t.drop();

    function getPlansForCacheEntry(query, sort, projection) {
        let key = {query: query, sort: sort, projection: projection};
        let res = t.runCommand('planCacheListPlans', key);
        assert.commandWorked(res, 'planCacheListPlans(' + tojson(key, '', true) + ' failed');
        assert(res.hasOwnProperty('plans'),
               'plans missing from planCacheListPlans(' + tojson(key, '', true) + ') result');
        return res;
    }

    // Assert that timeOfCreation exists in the cache entry. The difference between the current time
    // and the time a plan was cached should not be larger than an hour.
    function checkTimeOfCreation(query, sort, projection, date) {
        let key = {query: query, sort: sort, projection: projection};
        let res = t.runCommand('planCacheListPlans', key);
        assert.commandWorked(res, 'planCacheListPlans(' + tojson(key, '', true) + ' failed');
        assert(res.hasOwnProperty('timeOfCreation'),
               'timeOfCreation missing from planCacheListPlans');
        let kMillisecondsPerHour = 1000 * 60 * 60;
        assert.lte(Math.abs(date - res.timeOfCreation.getTime()),
                   kMillisecondsPerHour,
                   'timeOfCreation value is incorrect');
    }

    assert.commandWorked(t.save({a: 1, b: 1}));
    assert.commandWorked(t.save({a: 1, b: 2}));
    assert.commandWorked(t.save({a: 1, b: 2}));
    assert.commandWorked(t.save({a: 2, b: 2}));

    // We need two indices so that the MultiPlanRunner is executed.
    assert.commandWorked(t.ensureIndex({a: 1}));
    assert.commandWorked(t.ensureIndex({a: 1, b: 1}));

    // Invalid key should be an error.
    assert.eq([],
              getPlansForCacheEntry({unknownfield: 1}, {}, {}).plans,
              'planCacheListPlans should return empty results on unknown query shape');

    // Create a cache entry.
    assert.eq(1,
              t.find({a: 1, b: 1}, {_id: 0, a: 1}).sort({a: -1}).itcount(),
              'unexpected document count');

    let now = (new Date()).getTime();
    checkTimeOfCreation({a: 1, b: 1}, {a: -1}, {_id: 0, a: 1}, now);

    // Retrieve plans for valid cache entry.
    let entry = getPlansForCacheEntry({a: 1, b: 1}, {a: -1}, {_id: 0, a: 1});
    assert(entry.hasOwnProperty('works'),
           'works missing from planCacheListPlans() result ' + tojson(entry));
    assert.eq(entry.isActive, false);

    let plans = entry.plans;
    assert.eq(2, plans.length, 'unexpected number of plans cached for query');

    // Print every plan.
    // Plan details/feedback verified separately in section after Query Plan Revision tests.
    print('planCacheListPlans result:');
    for (let i = 0; i < plans.length; i++) {
        print('plan ' + i + ': ' + tojson(plans[i]));
    }

    // Test the queryHash and planCacheKey property by comparing entries for two different
    // query shapes.
    assert.eq(0, t.find({a: 132}).sort({b: -1, a: 1}).itcount(), 'unexpected document count');
    let entryNewShape = getPlansForCacheEntry({a: 123}, {b: -1, a: 1}, {});
    assert.eq(entry.hasOwnProperty("queryHash"), true);
    assert.eq(entryNewShape.hasOwnProperty("queryHash"), true);
    assert.neq(entry["queryHash"], entryNewShape["queryHash"]);
    assert.eq(entry.hasOwnProperty("planCacheKey"), true);
    assert.eq(entryNewShape.hasOwnProperty("planCacheKey"), true);
    assert.neq(entry["planCacheKey"], entryNewShape["planCacheKey"]);

    //
    // Tests for plan reason and feedback in planCacheListPlans
    //

    // Generate more plans for test query by adding indexes (compound and sparse).  This will also
    // clear the plan cache.
    assert.commandWorked(t.ensureIndex({a: -1}, {sparse: true}));
    assert.commandWorked(t.ensureIndex({a: 1, b: 1}));

    // Implementation note: feedback stats is calculated after 20 executions. See
    // PlanCacheEntry::kMaxFeedback.
    let numExecutions = 100;
    for (let i = 0; i < numExecutions; i++) {
        assert.eq(0, t.find({a: 3, b: 3}, {_id: 0, a: 1}).sort({a: -1}).itcount(), 'query failed');
    }

    now = (new Date()).getTime();
    checkTimeOfCreation({a: 3, b: 3}, {a: -1}, {_id: 0, a: 1}, now);

    entry = getPlansForCacheEntry({a: 3, b: 3}, {a: -1}, {_id: 0, a: 1});
    assert(entry.hasOwnProperty('works'), 'works missing from planCacheListPlans() result');
    assert.eq(entry.isActive, true);
    plans = entry.plans;

    // This should be obvious but feedback is available only for the first (winning) plan.
    print('planCacheListPlans result (after adding indexes and completing 20 executions):');
    for (let i = 0; i < plans.length; i++) {
        print('plan ' + i + ': ' + tojson(plans[i]));
        assert.gt(plans[i].reason.score, 0, 'plan ' + i + ' score is invalid');
        if (i > 0) {
            assert.lte(plans[i].reason.score,
                       plans[i - 1].reason.score,
                       'plans not sorted by score in descending order. ' +
                           'plan ' + i +
                           ' has a score that is greater than that of the previous plan');
        }
        assert(plans[i].reason.stats.hasOwnProperty('stage'), 'no stats inserted for plan ' + i);
    }
})();
