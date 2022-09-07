/**
 * Reproducer for an integer overflow bug in $_internalBoundedSort.
 *
 * @tags: [
 *   # Explain of a resolved view must be executed by mongos.
 *   directly_against_shardsvrs_incompatible,
 *   # Refusing to run a test that issues an aggregation command with explain because it may return
 *   # incomplete results if interrupted by a stepdown.
 *   does_not_support_stepdowns,
 *   # We need a timeseries collection.
 *   requires_timeseries,
 * ]
 */
(function() {
"use strict";

load('jstests/libs/analyze_plan.js');
load("jstests/core/timeseries/libs/timeseries.js");

if (!TimeseriesTest.bucketUnpackWithSortEnabled(db.getMongo())) {
    jsTestLog("Skipping test because 'BucketUnpackWithSort' is disabled.");
    return;
}

const coll = db.timeseries_internal_bounded_sort_overflow;
const buckets = db['system.buckets.' + coll.getName()];
coll.drop();
assert.commandWorked(
    db.createCollection(coll.getName(), {timeseries: {timeField: 't', metaField: 'm'}}));
const unpackStage = getAggPlanStage(coll.explain().aggregate(), '$_internalUnpackBucket');
assert(unpackStage.$_internalUnpackBucket);

// Insert some data: two events far enough apart that their difference in ms can overflow an int.
const docs = [
    {t: ISODate('2000-01-01T00:00:00Z')},
    {t: ISODate('2000-02-01T00:00:00Z')},
];
assert.commandWorked(coll.insert(docs));

// Make sure $_internalBoundedSort accepts it.
const result = buckets
                   .aggregate([
                       {$sort: {'control.min.t': 1}},
                       unpackStage,
                       {$_internalBoundedSort: {sortKey: {t: 1}, bound: {base: "min"}}},
                   ])
                   .toArray();

// Make sure the result is in order.
assert.eq(result[0].t, docs[0].t);
assert.eq(result[1].t, docs[1].t);
})();
