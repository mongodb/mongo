/**
 * Tests that time-series inserts occurring before a bucket is inserted go into the same bucket if
 * they are within the time range, regardless of the order in which they are inserted.
 *
 * @tags: [
 *     assumes_no_implicit_collection_creation_after_drop,
 *     does_not_support_retryable_writes,  # Batches containing more than one measurement
 *     does_not_support_stepdowns,
 *     requires_fcv_49,
 *     requires_find_command,
 *     requires_getmore,
 *     sbe_incompatible,
 * ]
 */
(function() {
'use strict';

load('jstests/core/timeseries/libs/timeseries.js');

if (!TimeseriesTest.timeseriesCollectionsEnabled(db.getMongo())) {
    jsTestLog('Skipping test because the time-series collection feature flag is disabled');
    return;
}

const collNamePrefix = 'timeseries_out_of_order_';

const timeFieldName = 'time';
const times = [
    ISODate('2021-01-01T01:00:00Z'),
    ISODate('2021-01-01T01:30:00Z'),
    ISODate('2021-01-01T02:00:00Z')
];
let docs = [{_id: 0, [timeFieldName]: times[1]}, {_id: 1, [timeFieldName]: times[0]}];

let collCount = 0;
const runTest = function(bucketsFn) {
    const coll = db.getCollection(collNamePrefix + collCount++);
    const bucketsColl = db.getCollection('system.buckets.' + coll.getName());
    coll.drop();

    assert.commandWorked(
        db.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName}}));
    assert.contains(bucketsColl.getName(), db.getCollectionNames());

    assert.commandWorked(coll.insert(docs, {ordered: false}));
    assert.docEq(coll.find().sort({_id: 1}).toArray(), docs);

    const buckets = bucketsColl.find().sort({_id: 1}).toArray();
    jsTestLog('Checking buckets:' + tojson(buckets));
    bucketsFn(buckets);
};

runTest(buckets => {
    assert.eq(buckets.length, 1);
    assert.eq(buckets[0].control.min[timeFieldName], times[0]);
    assert.eq(buckets[0].control.max[timeFieldName], times[1]);
});

docs.push({_id: 2, [timeFieldName]: times[2]});
runTest(buckets => {
    assert.eq(buckets.length, 2);
    assert.eq(buckets[0].control.min[timeFieldName], times[0]);
    assert.eq(buckets[0].control.max[timeFieldName], times[1]);
    assert.eq(buckets[1].control.min[timeFieldName], times[2]);
    assert.eq(buckets[1].control.max[timeFieldName], times[2]);
});

docs = [{_id: 0, [timeFieldName]: times[2]}, {_id: 1, [timeFieldName]: times[0]}];
runTest(buckets => {
    assert.eq(buckets.length, 2);
    assert.eq(buckets[0].control.min[timeFieldName], times[0]);
    assert.eq(buckets[0].control.max[timeFieldName], times[0]);
    assert.eq(buckets[1].control.min[timeFieldName], times[2]);
    assert.eq(buckets[1].control.max[timeFieldName], times[2]);
});
})();