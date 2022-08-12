/**
 * This tests that partialFilterExpressions can include the $internalBucketGeoWithin operator when
 * indexing buckets of timeseries operators.
 * @tags: [
 * requires_non_retryable_writes,
 * requires_fcv_51,
 * assumes_no_implicit_collection_creation_after_drop,
 *  does_not_support_transactions
 * ]
 */

load("jstests/libs/analyze_plan.js");
load("jstests/libs/feature_flag_util.js");
load('jstests/noPassthrough/libs/index_build.js');

(function() {
if (FeatureFlagUtil.isEnabled(db, "TimeseriesMetricIndexes")) {
    const timeFieldName = "timestamp";

    const coll = db.partialFilterExpression_with_internalBucketGeoWithin;
    const bucketsColl = db.getCollection('system.buckets.' + coll.getName());
    coll.drop();

    assert.commandWorked(
        db.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName}}));
    assert.contains(bucketsColl.getName(), db.getCollectionNames());

    assert.commandWorked(coll.insert({
        timestamp: ISODate(),
        a: 1,
        name: "Dallas",
        loc: {type: "Point", coordinates: [-96.808891, 32.779]}
    }));
    assert.commandWorked(coll.insert({
        timestamp: ISODate(),
        a: 1,
        name: "Paris TX",
        loc: {type: "Point", coordinates: [-95.555513, 33.6609389]}
    }));
    assert.commandWorked(coll.insert({
        timestamp: ISODate(),
        a: 2,
        name: "Houston",
        loc: {type: "Point", coordinates: [-95.3632715, 29.7632836]}
    }));
    assert.commandWorked(coll.insert({
        timestamp: ISODate(),
        a: 1,
        name: "San Antonio",
        loc: {type: "Point", coordinates: [-98.4936282, 29.4241219]}
    }));
    assert.commandWorked(coll.insert({
        timestamp: ISODate(),
        a: 3,
        name: "LA",
        loc: {type: "Point", coordinates: [-118.2436849, 34.0522342]}
    }));
    assert.commandWorked(coll.insert({
        timestamp: ISODate(),
        a: 3,
        name: "Berkeley",
        loc: {type: "Point", coordinates: [-122.272747, 37.8715926]}
    }));
    assert.commandWorked(coll.insert({
        timestamp: ISODate(),
        a: 1,
        name: "NYC",
        loc: {type: "Point", coordinates: [-74.0059729, 40.7142691]}
    }));

    var southWestUSPolygon = {
        type: "Polygon",
        coordinates: [[
            [-97.516473, 26.02054],
            [-106.528371, 31.895644],
            [
                -118.646927,
                33.748207,
            ],
            [-119.591751, 34.348991],
            [-103.068314, 36.426696],
            [-100.080033, 36.497382],
            [-99.975048, 34.506004],
            [-94.240190, 33.412542],
            [-94.075400, 29.725640],
            [-97.516473, 26.02054]
        ]]
    };

    var texasPolygon = {
        type: "Polygon",
        coordinates: [[
            [-97.516473, 26.02054],
            [-106.528371, 31.895644],
            [-103.034724, 31.932947],
            [-103.068314, 36.426696],
            [-100.080033, 36.497382],
            [-99.975048, 34.506004],
            [-94.240190, 33.412542],
            [-94.075400, 29.725640],
            [-97.516473, 26.02054]
        ]],
    };
    // Create index to cover Texas and Southern California.
    assert.commandWorked(bucketsColl.createIndex({a: 1}, {
        partialFilterExpression: {
            $_internalBucketGeoWithin: {
                withinRegion: {
                    $geometry: southWestUSPolygon

                },
                field: "loc"
            }
        }
    }));

    IndexBuildTest.assertIndexes(bucketsColl, 1, ["a_1"]);

    var findAndExplain = assert.commandWorked(bucketsColl
                                                  .find({
                                                      a: 1,
                                                      $_internalBucketGeoWithin: {
                                                          withinRegion: {
                                                              $geometry: texasPolygon

                                                          },
                                                          field: "loc"
                                                      }
                                                  })
                                                  .explain());

    assert(isIxscan(db, getWinningPlan(findAndExplain.queryPlanner)));

    // Unlike the example above, this query provides a different argument for "field" than what we
    // indexed the collection on. In this case, we cannot use our index and expect to have to do a
    // collection scan.
    findAndExplain = assert.commandWorked(bucketsColl
                                              .find({
                                                  a: 1,
                                                  $_internalBucketGeoWithin: {
                                                      withinRegion: {
                                                          $geometry: texasPolygon

                                                      },
                                                      field: "geoloc"
                                                  }
                                              })
                                              .explain());

    assert(isCollscan(db, getWinningPlan(findAndExplain.queryPlanner)));
    assert.commandWorked(bucketsColl.dropIndexes());

    // Create a smaller index and query for a larger region, resulting in a collection scan.
    assert.commandWorked(bucketsColl.createIndex({a: 1}, {
        partialFilterExpression: {
            $_internalBucketGeoWithin: {
                withinRegion: {
                    $geometry: texasPolygon

                },
                field: "loc"
            }
        }
    }));
    IndexBuildTest.assertIndexes(bucketsColl, 1, ["a_1"]);

    findAndExplain = assert.commandWorked(bucketsColl
                                              .find({
                                                  a: 1,
                                                  $_internalBucketGeoWithin: {
                                                      withinRegion: {
                                                          $geometry: southWestUSPolygon

                                                      },
                                                      field: "loc"
                                                  }
                                              })
                                              .explain());
    assert(isCollscan(db, getWinningPlan(findAndExplain.queryPlanner)));
}
})();
