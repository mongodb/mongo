/**
 * Tests that that metadata only updates and deletes do not succeed against closed buckets.
 *
 * @tags: [
 *   # This test depends on certain writes ending up in the same bucket. Stepdowns and tenant
 *   # migrations may result in writes splitting between two primaries, and thus different buckets.
 *   does_not_support_stepdowns,
 *   # We need a timeseries collection.
 *   requires_timeseries,
 * ]
 */
import {getTimeseriesCollForRawOps, kRawOperationSpec} from "jstests/core/libs/raw_operation_utils.js";
import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";

TimeseriesTest.run((insert) => {
    const collNamePrefix = jsTestName() + "_";

    const timeFieldName = "time";
    const metaFieldName = "meta";

    // create populated bucket
    let coll = db.getCollection(collNamePrefix);
    coll.drop();
    jsTestLog("Running metadata update/delete respects control.closed test");
    assert.commandWorked(
        db.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}),
    );
    // Ensure _id order of raw buckets documents by using constant times.
    let docs = [
        {_id: 0, time: ISODate("2020-11-26T00:00:00.000Z"), meta: "a", x: 0},
        {_id: 1, time: ISODate("2020-11-26T00:10:00.000Z"), meta: "a", x: 10},
    ];
    assert.commandWorked(insert(coll, docs), "failed to insert docs: " + tojson(docs));
    assert.eq(2, coll.find({}).toArray().length);
    assert.eq(2, coll.find({"meta": "a"}).toArray().length);
    assert.eq(0, coll.find({"meta": "b"}).toArray().length);

    // close bucket
    getTimeseriesCollForRawOps(coll).findAndModify({
        query: {"meta": "a"},
        update: {$set: {"control.closed": true}},
        ...kRawOperationSpec,
    });

    // should be a no-op
    assert.commandWorked(coll.updateMany({"meta": {$eq: "a"}}, {$set: {"meta": "b"}}));
    assert.eq(2, coll.find({"meta": "a"}).toArray().length);
    assert.eq(0, coll.find({"meta": "b"}).toArray().length);

    // should be a no-op
    assert.commandWorked(coll.deleteMany({"meta": {$eq: "a"}}));
    assert.eq(2, coll.find({"meta": "a"}).toArray().length);
    assert.eq(0, coll.find({"meta": "b"}).toArray().length);

    // populate three closed buckets
    docs = [
        {_id: 2, time: ISODate("2020-11-26T00:20:00.000Z"), meta: "a", x: 20},
        {_id: 3, time: ISODate("2020-11-26T00:30:00.000Z"), meta: "a", x: 30},
    ];
    assert.commandWorked(insert(coll, docs), "failed to insert docs: " + tojson(docs));
    getTimeseriesCollForRawOps(coll).findAndModify({
        query: {"meta": "a"},
        update: {$set: {"control.closed": true}},
        ...kRawOperationSpec,
    });
    docs = [
        {_id: 4, time: ISODate("2020-11-26T00:40:00.000Z"), meta: "a", x: 40},
        {_id: 5, time: ISODate("2020-11-26T00:50:00.000Z"), meta: "a", x: 50},
    ];
    assert.commandWorked(insert(coll, docs), "failed to insert docs: " + tojson(docs));
    getTimeseriesCollForRawOps(coll).findAndModify({
        query: {"meta": "a"},
        update: {$set: {"control.closed": true}},
        ...kRawOperationSpec,
    });
    assert.eq(6, coll.find({"meta": "a"}).toArray().length);
    assert.eq(0, coll.find({"meta": "b"}).toArray().length);

    // should delete the bucket in rawData mode
    assert.commandWorked(
        getTimeseriesCollForRawOps(coll).deleteOne(
            {
                "meta": "a",
            },
            kRawOperationSpec,
        ),
    );
    assert.eq(4, coll.find({"meta": "a"}).toArray().length);
    assert.eq(1, coll.stats().timeseries.bucketCount, coll.stats().timeseries);
    assert.eq(0, coll.find({"meta": "b"}).toArray().length);
    assert.commandWorked(
        getTimeseriesCollForRawOps(coll).deleteMany(
            {
                "meta": "a",
            },
            kRawOperationSpec,
        ),
    );
    assert.eq(0, coll.find({"meta": "a"}).toArray().length);
    assert.eq(0, coll.stats().timeseries.bucketCount);
    assert.eq(0, coll.find({"meta": "b"}).toArray().length);
});
