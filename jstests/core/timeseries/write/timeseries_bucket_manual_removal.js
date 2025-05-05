/**
 * Tests that a time-series collection handles a bucket being manually removed from the buckets
 * collection.
 *
 * @tags: [
 *   # This test depends on certain writes ending up in the same bucket. Stepdowns and tenant
 *   # migrations may result in writes splitting between two primaries, and thus different buckets.
 *   does_not_support_stepdowns,
 *   # We need a timeseries collection.
 *   requires_timeseries,
 * ]
 */
import {
    getTimeseriesCollForRawOps,
    kRawOperationSpec
} from "jstests/core/libs/raw_operation_utils.js";
import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";

TimeseriesTest.run((insert) => {
    const coll = db[jsTestName()];

    const timeFieldName = 'time';

    coll.drop();
    assert.commandWorked(
        db.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName}}));

    const docs1 = [
        {
            _id: 0,
            [timeFieldName]: ISODate("2021-01-01T01:00:00Z"),
        },
        {
            _id: 1,
            [timeFieldName]: ISODate("2021-01-01T01:01:00Z"),
        },
    ];
    const docs2 = [
        {
            _id: 2,
            [timeFieldName]: ISODate("2021-01-01T01:02:00Z"),
        },
        {
            _id: 3,
            [timeFieldName]: ISODate("2021-01-01T01:03:00Z"),
        },
    ];

    assert.commandWorked(insert(coll, docs1));
    assert.docEq(docs1, coll.find().toArray());
    let buckets = getTimeseriesCollForRawOps(coll).find().rawData().toArray();
    assert.eq(buckets.length, 1, 'Expected one bucket but found ' + tojson(buckets));
    const bucketId = buckets[0]._id;

    assert.commandWorked(
        getTimeseriesCollForRawOps(coll).remove({_id: bucketId}, kRawOperationSpec));
    assert.docEq([], coll.find().toArray());
    buckets = getTimeseriesCollForRawOps(coll).find().rawData().toArray();
    assert.eq(buckets.length, 0, 'Expected no buckets but found ' + tojson(buckets));

    assert.commandWorked(
        getTimeseriesCollForRawOps(coll).remove({_id: bucketId}, kRawOperationSpec));
    assert.docEq([], coll.find().toArray());
    buckets = getTimeseriesCollForRawOps(coll).find().rawData().toArray();
    assert.eq(buckets.length, 0, 'Expected no buckets but found ' + tojson(buckets));

    assert.commandWorked(coll.insert(docs2, {ordered: false}));
    assert.docEq(docs2, coll.find().toArray());
    buckets = getTimeseriesCollForRawOps(coll).find().rawData().toArray();
    assert.eq(buckets.length, 1, 'Expected one bucket but found ' + tojson(buckets));
    assert.neq(buckets[0]._id, bucketId);
});
