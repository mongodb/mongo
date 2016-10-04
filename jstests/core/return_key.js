/**
 * Tests for returnKey.
 */
load("jstests/libs/analyze_plan.js");

(function() {
    'use strict';

    var results;
    var explain;

    var coll = db.jstests_returnkey;
    coll.drop();

    assert.writeOK(coll.insert({a: 1, b: 3}));
    assert.writeOK(coll.insert({a: 2, b: 2}));
    assert.writeOK(coll.insert({a: 3, b: 1}));

    assert.commandWorked(coll.ensureIndex({a: 1}));
    assert.commandWorked(coll.ensureIndex({b: 1}));

    // Basic returnKey.
    results = coll.find().hint({a: 1}).sort({a: 1}).returnKey().toArray();
    assert.eq(results, [{a: 1}, {a: 2}, {a: 3}]);
    results = coll.find().hint({a: 1}).sort({a: -1}).returnKey().toArray();
    assert.eq(results, [{a: 3}, {a: 2}, {a: 1}]);

    // Check that the plan is covered.
    explain = coll.find().hint({a: 1}).sort({a: 1}).returnKey().explain();
    assert(isIndexOnly(explain.queryPlanner.winningPlan));
    explain = coll.find().hint({a: 1}).sort({a: -1}).returnKey().explain();
    assert(isIndexOnly(explain.queryPlanner.winningPlan));

    // returnKey with an in-memory sort.
    results = coll.find().hint({a: 1}).sort({b: 1}).returnKey().toArray();
    assert.eq(results, [{a: 3}, {a: 2}, {a: 1}]);
    results = coll.find().hint({a: 1}).sort({b: -1}).returnKey().toArray();
    assert.eq(results, [{a: 1}, {a: 2}, {a: 3}]);

    // Check that the plan is not covered.
    explain = coll.find().hint({a: 1}).sort({b: 1}).returnKey().explain();
    assert(!isIndexOnly(explain.queryPlanner.winningPlan));
    explain = coll.find().hint({a: 1}).sort({b: -1}).returnKey().explain();
    assert(!isIndexOnly(explain.queryPlanner.winningPlan));

    // returnKey takes precedence over other a regular inclusion projection. Should still be
    // covered.
    results = coll.find({}, {b: 1}).hint({a: 1}).sort({a: -1}).returnKey().toArray();
    assert.eq(results, [{a: 3}, {a: 2}, {a: 1}]);
    explain = coll.find({}, {b: 1}).hint({a: 1}).sort({a: -1}).returnKey().explain();
    assert(isIndexOnly(explain.queryPlanner.winningPlan));

    // returnKey takes precedence over other a regular exclusion projection. Should still be
    // covered.
    results = coll.find({}, {a: 0}).hint({a: 1}).sort({a: -1}).returnKey().toArray();
    assert.eq(results, [{a: 3}, {a: 2}, {a: 1}]);
    explain = coll.find({}, {a: 0}).hint({a: 1}).sort({a: -1}).returnKey().explain();
    assert(isIndexOnly(explain.queryPlanner.winningPlan));

    // Unlike other projections, sortKey meta-projection can co-exist with returnKey.
    results =
        coll.find({}, {c: {$meta: 'sortKey'}}).hint({a: 1}).sort({a: -1}).returnKey().toArray();
    assert.eq(results, [{a: 3, c: {'': 3}}, {a: 2, c: {'': 2}}, {a: 1, c: {'': 1}}]);

    // returnKey with sortKey $meta where there is an in-memory sort.
    results =
        coll.find({}, {c: {$meta: 'sortKey'}}).hint({a: 1}).sort({b: 1}).returnKey().toArray();
    assert.eq(results, [{a: 3, c: {'': 1}}, {a: 2, c: {'': 2}}, {a: 1, c: {'': 3}}]);

    // returnKey with multiple sortKey $meta projections.
    results = coll.find({}, {c: {$meta: 'sortKey'}, d: {$meta: 'sortKey'}})
                  .hint({a: 1})
                  .sort({b: 1})
                  .returnKey()
                  .toArray();
    assert.eq(results, [
        {a: 3, c: {'': 1}, d: {'': 1}},
        {a: 2, c: {'': 2}, d: {'': 2}},
        {a: 1, c: {'': 3}, d: {'': 3}}
    ]);
})();
