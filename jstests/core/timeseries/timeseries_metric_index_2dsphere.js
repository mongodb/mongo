/**
 * Tests 2dsphere indexes on measurement fields in time-series collections
 *
 * Tests index creation, document insertion, index utilization for queries, and index drop.
 *
 * @tags: [
 *   does_not_support_stepdowns,
 *   does_not_support_transactions,
 *   # Explain of a resolved view must be executed by mongos.
 *   directly_against_shardsvrs_incompatible,
 * ]
 */

(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");
load("jstests/libs/analyze_plan.js");
load("jstests/libs/feature_flag_util.js");

if (!FeatureFlagUtil.isEnabled(db, "TimeseriesMetricIndexes")) {
    return;
}

TimeseriesTest.run((insert) => {
    const testdb = db.getSiblingDB("timeseries_metric_index_2dsphere_db");
    const timeseriescoll = testdb.getCollection("timeseries_metric_index_2dsphere_coll");
    const bucketscoll = testdb.getCollection('system.buckets.' + timeseriescoll.getName());

    const timeFieldName = 'tm';
    const metaFieldName = 'mm';

    /**
     * Sets up an empty time-series collection on namespace 'timeseriescoll' using 'timeFieldName'
     * and 'metaFieldName'. Checks that the buckets collection is created, as well.
     */
    function resetCollections() {
        timeseriescoll.drop();  // implicitly drops bucketscoll.

        assert.commandWorked(testdb.createCollection(
            timeseriescoll.getName(),
            {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));

        const dbCollNames = testdb.getCollectionNames();
        assert.contains(bucketscoll.getName(),
                        dbCollNames,
                        "Failed to find namespace '" + bucketscoll.getName() +
                            "' amongst: " + tojson(dbCollNames));
    }

    /**
     * Test 2dsphere index on time-series collection.
     */

    jsTestLog("Testing 2dsphere index on time-series collection.");
    resetCollections();

    // Create a 2dsphere index on the time-series collection.
    const twoDSphereTimeseriesIndexSpec = {'location': '2dsphere'};
    assert.commandWorked(
        timeseriescoll.createIndex(twoDSphereTimeseriesIndexSpec),
        'Failed to create a 2dsphere index with: ' + tojson(twoDSphereTimeseriesIndexSpec));

    // Insert a 2dsphere index usable document.
    const twoDSphereDocs = [
        {
            _id: 0,
            [timeFieldName]: ISODate("2022-04-01T00:00:00.000Z"),
            [metaFieldName]: "m1",
            location: {type: "Point", coordinates: [40, -70]}
        },
        {
            _id: 1,
            [timeFieldName]: ISODate("2022-04-01T00:01:00.000Z"),  // should land in same bucket
            [metaFieldName]: "m1",
            location: {type: "Point", coordinates: [40.1, -70.1]}
        },
        {
            _id: 2,
            [timeFieldName]: ISODate("2022-04-01T00:00:00.000Z"),
            [metaFieldName]: "m2",
            location: {type: "Point", coordinates: [40.2, -70.2]}
        },
        {
            _id: 3,
            [timeFieldName]: ISODate("2022-04-01T00:01:00.000Z"),  // should land in same bucket
            [metaFieldName]: "m2",
            location: {type: "Point", coordinates: [40.3, -70.3]}
        },
        {
            _id: 4,
            [timeFieldName]: ISODate("2022-04-01T00:02:00.000Z"),  // should land in same bucket
            [metaFieldName]: "m2",
            location: {type: "Point", coordinates: [40.4, -70.4]}
        },
        {
            _id: 5,
            [timeFieldName]:
                ISODate("2022-04-01T02:00:00.000Z"),  // should open new bucket and compress old one
            [metaFieldName]: "m2",
            location: {type: "Point", coordinates: [40.5, -70.5]}
        },
        {
            _id: 6,
            [timeFieldName]: ISODate("2022-04-01T02:01:00.000Z"),  // should land in same bucket
            [metaFieldName]: "m2"
        },
        {_id: 7, [timeFieldName]: ISODate("2022-04-01T00:00:00.000Z"), [metaFieldName]: "m3"},
    ];
    assert.commandWorked(insert(timeseriescoll, twoDSphereDocs),
                         'Failed to insert twoDSphereDocs: ' + tojson(twoDSphereDocs));
    assert.eq(bucketscoll.count(), 4);
    printjson(bucketscoll.find({}).toArray());

    // Test invalid documents
    const docWithInvalidCoordinates = {
        _id: 6,
        [timeFieldName]: ISODate(),
        location: {type: "Polygon", coordinates: [[0, 0], [1, 0], [1, 1], [0, 0]]}
    };
    // Can't extract geo keys: GeoJSON coordinates must be an array
    assert.commandFailedWithCode(insert(timeseriescoll, docWithInvalidCoordinates), 183934);
    const docsWithTypeNotPoint = [
        {
            _id: 7,
            [timeFieldName]: ISODate(),
            location: {type: "MultiPoint", coordinates: [[0, 0], [1, 0], [1, 1], [0, 0]]}
        },
        {
            _id: 8,
            [timeFieldName]: ISODate(),
            location: {type: "LineString", coordinates: [[0, 0], [1, 0], [1, 1], [0, 0]]}
        }
    ];
    docsWithTypeNotPoint.forEach((invalidDoc) => {
        // Time-series collections '2dsphere' indexes only support point data
        assert.commandFailedWithCode(insert(timeseriescoll, invalidDoc), 183493);
    });

    // Assert that geoWithin queries use the index.
    const geoWithinPlan =
        timeseriescoll.find({location: {$geoWithin: {$centerSphere: [[40, -70], 0.0025]}}})
            .explain();
    assert.neq(null, getAggPlanStage(geoWithinPlan, "IXSCAN"), geoWithinPlan);
    // And that their results are correct.
    assert.eq(2,
              timeseriescoll.find({location: {$geoWithin: {$centerSphere: [[40, -70], 0.0025]}}})
                  .toArray()
                  .length,
              geoWithinPlan);
    // Assert 2d queries don't use the 2dsphere index.
    const geoWithinPlan2d =
        timeseriescoll.find({location: {$geoWithin: {$center: [[40, -70], .15]}}}).explain();
    assert.neq(null, getAggPlanStage(geoWithinPlan2d, "COLLSCAN"), geoWithinPlan2d);
    // And that their results are correct.
    assert.eq(
        2,
        timeseriescoll.find({location: {$geoWithin: {$center: [[40, -70], .15]}}}).toArray().length,
        geoWithinPlan2d);

    assert.eq(6,
              timeseriescoll
                  .aggregate([{
                      $geoNear: {
                          near: {type: "Point", coordinates: [40.4, -70.4]},
                          distanceField: "dist",
                          spherical: true,
                          key: 'location'
                      }
                  }])
                  .toArray()
                  .length,
              "Failed to use 2dsphere index: " + tojson(twoDSphereTimeseriesIndexSpec));

    assert.commandWorked(timeseriescoll.dropIndex(twoDSphereTimeseriesIndexSpec));
});
})();
