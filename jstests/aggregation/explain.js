// Tests the behavior of explain() when used with the aggregation
// pipeline.  Explain() should not read or modify the plan cache.
(function() {
    "use strict";

    load('jstests/libs/analyze_plan.js');  // For getAggPlanStage().

    let coll = db.explain;
    coll.drop();

    assert.commandWorked(coll.createIndex({x: 1}));
    assert.commandWorked(coll.createIndex({y: 1}));

    let result = coll.explain().aggregate([{$match: {x: 1, y: 1}}]);
    assert.eq(null, getAggPlanStage(result, "CACHED_PLAN"));

    // At this point, there should be no entries in the plan cache.
    result = coll.explain().aggregate([{$match: {x: 1, y: 1}}]);
    assert.eq(null, getAggPlanStage(result, "CACHED_PLAN"));

    // Now add entry in the cache without explain().
    result = coll.aggregate([{$match: {x: 1, y: 1}}]);

    // Now there's an entry in the cache, make sure explain() doesn't use it.
    result = coll.explain().aggregate([{$match: {x: 1, y: 1}}]);
    assert.eq(null, getAggPlanStage(result, "CACHED_PLAN"));

})();
