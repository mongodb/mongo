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
        db.createCollection(coll.getName(),
                            {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}),
    );
    let bucketsColl = db.getCollection("system.buckets." + coll.getName());
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
    bucketsColl.findAndModify({
        query: {"meta": "a"},
        update: {$set: {"control.closed": true}},
    });

    // should be a no-op
    assert.commandWorked(coll.updateMany({"meta": {$eq: "a"}}, {$set: {"meta": "b"}}));
    assert.eq(2, coll.find({"meta": "a"}).toArray().length);
    assert.eq(0, coll.find({"meta": "b"}).toArray().length);

    // should be a no-op
    assert.commandWorked(coll.deleteMany({"meta": {$eq: "a"}}));
    assert.eq(2, coll.find({"meta": "a"}).toArray().length);
    assert.eq(0, coll.find({"meta": "b"}).toArray().length);
});
