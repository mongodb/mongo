// From the work done for SERVER-22093, an aggregation pipeline that does not require any fields
// from the input documents will tell the query planner to use a count scan, which is faster than an
// index scan. In this test file, we check this behavior through explain().
//
// This test assumes that an initial $match will be absorbed by the query system, which will not
// happen if the $match is wrapped within a $facet stage.
// @tags: [do_not_wrap_aggregations_in_facets]
load('jstests/libs/analyze_plan.js');

(function() {
    "use strict";

    var coll = db.countscan;
    coll.drop();

    for (var i = 0; i < 3; i++) {
        for (var j = 0; j < 10; j += 2) {
            coll.insert({foo: i, bar: j});
        }
    }

    coll.ensureIndex({foo: 1});

    var simpleGroup = coll.aggregate([{$group: {_id: null, count: {$sum: 1}}}]).toArray();

    assert.eq(simpleGroup.length, 1);
    assert.eq(simpleGroup[0]["count"], 15);

    var explained = coll.explain().aggregate(
        [{$match: {foo: {$gt: 0}}}, {$group: {_id: null, count: {$sum: 1}}}]);

    assert(planHasStage(explained.stages[0].$cursor.queryPlanner.winningPlan, "COUNT_SCAN"));

    explained = coll.explain().aggregate([
        {$match: {foo: {$gt: 0}}},
        {$project: {_id: 0, a: {$literal: null}}},
        {$group: {_id: null, count: {$sum: 1}}}
    ]);

    assert(planHasStage(explained.stages[0].$cursor.queryPlanner.winningPlan, "COUNT_SCAN"));

    // Make sure a $count stage can use the COUNT_SCAN optimization.
    explained = coll.explain().aggregate([{$match: {foo: {$gt: 0}}}, {$count: "count"}]);
    assert(planHasStage(explained.stages[0].$cursor.queryPlanner.winningPlan, "COUNT_SCAN"));

    // A $match that is not a single range cannot use the COUNT_SCAN optimization.
    explained = coll.explain().aggregate([{$match: {foo: {$in: [0, 1]}}}, {$count: "count"}]);
    assert(!planHasStage(explained.stages[0].$cursor.queryPlanner.winningPlan, "COUNT_SCAN"));
}());
