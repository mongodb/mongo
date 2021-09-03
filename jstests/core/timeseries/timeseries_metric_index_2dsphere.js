/**
 * Tests 2dsphere indexes on measurement fields in time-series collections
 *
 * Tests index creation, document insertion, index utilization for queries, and index drop.
 *
 * @tags: [
 *   assumes_no_implicit_collection_creation_after_drop,
 *   does_not_support_stepdowns,
 *   does_not_support_transactions,
 *   requires_fcv_51,
 *   requires_getmore,
 * ]
 */

(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");
load("jstests/libs/analyze_plan.js");

if (!TimeseriesTest.timeseriesMetricIndexesEnabled(db.getMongo())) {
    return;
}

TimeseriesTest.run((insert) => {
    const testdb = db.getSiblingDB("timeseries_special_indexes_db");
    const timeseriescoll = testdb.getCollection("timeseries_special_indexes_coll");
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
            [timeFieldName]: ISODate(),
            [metaFieldName]: "m1",
            location: {type: "Point", coordinates: [40, -70]}
        },
        {
            _id: 1,
            [timeFieldName]: ISODate(),
            [metaFieldName]: "m1",
            location: {type: "Point", coordinates: [40.1, -70.1]}
        },
        {
            _id: 2,
            [timeFieldName]: ISODate(),
            [metaFieldName]: "m2",
            location: {type: "Point", coordinates: [40.2, -70.2]}
        },
        {
            _id: 3,
            [timeFieldName]: ISODate(),
            [metaFieldName]: "m2",
            location: {type: "Point", coordinates: [40.2, -70.2]}
        },
        {_id: 4, [timeFieldName]: ISODate(), [metaFieldName]: "m2"},
        {_id: 5, [timeFieldName]: ISODate(), [metaFieldName]: "m3"},
    ];
    assert.commandWorked(insert(timeseriescoll, twoDSphereDocs),
                         'Failed to insert twoDSphereDocs: ' + tojson(twoDSphereDocs));

    const invalidDocs = [
        {
            _id: 6,
            [timeFieldName]: ISODate(),
            location: {type: "Polygon", coordinates: [[0, 0], [1, 0], [1, 1], [0, 0]]}
        },
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
    for (const invalidDoc in invalidDocs) {
        assert.commandFailedWithCode(assert.throws(() => insert(timeseriescoll, invalidDoc)),
                                                  ErrorCodes.BadValue);
    }

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

    /* TODO (SERVER-58602): Enable this test once query planner can use 'GEO_2DSPHERE_BUCKET' index
    with $geoNear by translating to a $geoWithin + $sort assert.eq(2, bucketscoll .aggregate([{
                      $geoNear: {
                          near: {type: "Point", coordinates: [40.4, -70.4]},
                          distanceField: "dist",
                          spherical: true,
                          key: 'data.location'
                      }
                  }])
                  .toArray()
                  .length,
              "Failed to use 2dsphere index: " + tojson(twoDSphereBucketsIndexSpec));*/

    assert.commandWorked(timeseriescoll.dropIndex(twoDSphereTimeseriesIndexSpec));
});
})();
