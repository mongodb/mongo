/**
 * Test some special cases of $match on time-series collections.
 *
 * @tags: [
 *   # explain.stages hidden when run against sharded collections.
 *   assumes_unsharded_collection,
 *   does_not_support_stepdowns,
 *   does_not_support_transactions,
 *   requires_fcv_50,
 *   requires_timeseries,
 *   requires_pipeline_optimization,
 * ]
 */

(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");
load("jstests/libs/analyze_plan.js");

if (!TimeseriesTest.timeseriesCollectionsEnabled(db.getMongo())) {
    jsTestLog("Skipping test because the time-series collection feature flag is disabled");
    return;
}

const coll = db.getCollection(jsTestName());
coll.drop();
assert.commandWorked(
    db.createCollection(coll.getName(), {timeseries: {timeField: 'time', metaField: 'tag'}}));

const docDate = new Date("2019-03-09T07:29:34.201Z");
assert.commandWorked(coll.insertMany([
    {_id: 2, time: new Date("2019-07-29T07:46:38.746Z"), tag: 2, measurement0: 2.03},
    {_id: 4, time: new Date("2019-11-29T12:20:34.821Z"), tag: 1, measurement0: 4.9},
    {_id: 5, time: docDate, tag: 2, measurement0: 5.5, measurement1: 7.5}
]));

const result = coll.aggregate([{"$match": {"$expr": {"$getField": "measurement1"}}}]).toArray();

assert.docEq(result, [{_id: 5, time: docDate, tag: 2, measurement0: 5.5, measurement1: 7.5}]);

// Check that the optimized plan does not push the $match before $internalUnpackBucket
// as a filter in the collection scan.
function getWinningPlanForPipeline(coll, pipeline) {
    const explain = assert.commandWorked(coll.explain().aggregate(pipeline));
    if ("queryPlanner" in explain) {
        return getWinningPlan(explain.queryPlanner);
    }
    return getWinningPlan(explain.stages[0].$cursor.queryPlanner);
}

const winningPlan =
    getWinningPlanForPipeline(coll, [{"$match": {"$expr": {"$getField": "measurement1"}}}]);
const collScan = getPlanStage(winningPlan, "COLLSCAN");
assert(collScan);
assert(!collScan.filter || Object.keys(collScan.filter).length == 0);
})();
