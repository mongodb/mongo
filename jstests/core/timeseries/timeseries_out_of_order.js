/**
 * Tests that time-series inserts occurring before a bucket is inserted go into the same bucket if
 * they are within the time range, regardless of the order in which they are inserted.
 *
 * @tags: [
 *   # This test depends on certain writes ending up in the same bucket. Stepdowns may result in
 *   # writes splitting between two primaries, and thus different buckets.
 *   does_not_support_stepdowns,
 *   # We need a timeseries collection.
 *   requires_timeseries,
 * ]
 */
(function() {
'use strict';

load('jstests/core/timeseries/libs/timeseries.js');

TimeseriesTest.run((insert) => {
    const collNamePrefix = 'timeseries_out_of_order_';

    const timeFieldName = 'time';
    const times = [
        ISODate('2021-01-01T01:00:00Z'),
        ISODate('2021-01-01T01:00:30Z'),
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

        assert.commandWorked(insert(coll, docs));
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
});
})();
