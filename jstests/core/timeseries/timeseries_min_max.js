/**
 * Tests that a time-series bucket's control.min and control.max accurately reflect the minimum and
 * maximum values inserted into the bucket.
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
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");  // For 'TimeseriesTest'.

TimeseriesTest.run((insert) => {
    const collNamePrefix = 'timeseries_min_max_';

    const timeFieldName = 'time';
    const metaFieldName = 'meta';

    let collCount = 0;
    let coll;
    let bucketsColl;

    const clearColl = function() {
        coll = db.getCollection(collNamePrefix + collCount++);
        bucketsColl = db.getCollection('system.buckets.' + coll.getName());

        coll.drop();

        const timeFieldName = 'time';
        assert.commandWorked(db.createCollection(
            coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));
        assert.contains(bucketsColl.getName(), db.getCollectionNames());
    };
    clearColl();

    const runTest = function(doc, expectedMin, expectedMax) {
        doc[timeFieldName] = ISODate();
        assert.commandWorked(insert(coll, doc));

        // Find the _id value of the measurement just inserted.
        const matchingMeasurements = coll.find(doc).toArray();
        assert.eq(matchingMeasurements.length, 1);
        const measurementId = matchingMeasurements[0]._id;

        // Find the bucket the measurement belongs to.
        const bucketDocs = bucketsColl
                               .find({
                                   $and: [
                                       {"control.min._id": {$lte: measurementId}},
                                       {"control.max._id": {$gte: measurementId}}
                                   ]
                               },
                                     {
                                         'control.min._id': 0,
                                         'control.max._id': 0,
                                         ['control.min.' + timeFieldName]: 0,
                                         ['control.max.' + timeFieldName]: 0
                                     })
                               .toArray();
        assert.eq(bucketDocs.length, 1);

        const bucketDoc = bucketDocs[0];
        jsTestLog('Bucket collection document: ' + tojson(bucketDoc));

        assert.docEq(
            expectedMin, bucketDoc.control.min, 'invalid min in bucket: ' + tojson(bucketDoc));
        assert.docEq(
            expectedMax, bucketDoc.control.max, 'invalid max in bucket: ' + tojson(bucketDoc));
    };

    // Empty objects are considered.
    runTest({a: {}}, {a: {}}, {a: {}});
    runTest({a: {x: {}}}, {a: {x: {}}}, {a: {x: {}}});
    runTest({a: {x: {y: 1}}}, {a: {x: {y: 1}}}, {a: {x: {y: 1}}});
    runTest({a: {x: {}}}, {a: {x: {y: 1}}}, {a: {x: {y: 1}}});
    clearColl();

    // The metadata field is not considered.
    runTest({meta: 1}, {}, {});
    clearColl();

    // Objects and arrays are updated element-wise.
    runTest(
        {a: {x: 1, y: 2}, b: [1, 2]}, {a: {x: 1, y: 2}, b: [1, 2]}, {a: {x: 1, y: 2}, b: [1, 2]});
    runTest(
        {a: {x: 2, y: 1}, b: [2, 1]}, {a: {x: 1, y: 1}, b: [1, 1]}, {a: {x: 2, y: 2}, b: [2, 2]});

    // Schema change, new bucket.
    runTest({a: {x: {z: [3, 4]}}, b: [{x: 3, y: 4}]},
            {a: {x: {z: [3, 4]}}, b: [{x: 3, y: 4}]},
            {a: {x: {z: [3, 4]}}, b: [{x: 3, y: 4}]});
    // Sub-objects and arrays also updated element-wise
    runTest({a: {x: {z: [4, 3]}}, b: [{x: 4, y: 3}, 3, 1]},
            {a: {x: {z: [3, 3]}}, b: [{x: 3, y: 3}, 3, 1]},
            {a: {x: {z: [4, 4]}}, b: [{x: 4, y: 4}, 3, 1]});
    clearColl();

    // If the two types being compared are not both objects or both arrays, we'll detect a schema
    // change and open a new bucket.
    runTest({a: 1}, {a: 1}, {a: 1});
    runTest({a: {b: 1}}, {a: {b: 1}}, {a: {b: 1}});
    runTest({a: []}, {a: []}, {a: []});
    runTest({a: [5]}, {a: [5]}, {a: [5]});
    clearColl();

    // We correctly detect schema changes from Array to Object and back
    runTest({a: []}, {a: []}, {a: []});
    runTest({a: {b: 1}}, {a: {b: 1}}, {a: {b: 1}});
    runTest({a: [5]}, {a: [5]}, {a: [5]});
    clearColl();

    // Sparse measurements only affect the min/max for the fields present.
    runTest({a: 1, c: 1}, {a: 1, c: 1}, {a: 1, c: 1});
    runTest({b: 2}, {a: 1, b: 2, c: 1}, {a: 1, b: 2, c: 1});
    runTest({c: 3, d: 3}, {a: 1, b: 2, c: 1, d: 3}, {a: 1, b: 2, c: 3, d: 3});
    clearColl();

    // We correctly handle canonical type
    runTest({a: Number(1.5)}, {a: Number(1.5)}, {a: Number(1.5)});
    runTest({a: NumberLong(2)}, {a: Number(1.5)}, {a: NumberLong(2)});
    runTest({a: NumberInt(1)}, {a: NumberInt(1)}, {a: NumberLong(2)});
    runTest({a: NumberDecimal(2.5)}, {a: NumberInt(1)}, {a: NumberDecimal(2.5)});
    runTest({a: Number(0.5)}, {a: Number(0.5)}, {a: NumberDecimal(2.5)});
});
})();
