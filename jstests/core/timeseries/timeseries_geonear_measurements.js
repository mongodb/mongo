/**
 * Test the behavior of $geoNear queries on time-series measurements.
 *
 * $geoNear has a lot of options, which can combine in surprising ways. For example, specifying
 * the query point as GeoJSON implicitly makes it a spherical query, but with different units
 * than if you pass 'spherical: true' explicitly.
 *
 * To ensure we get these cases right, this test runs all of its assertions on a normal collection
 * first, to ensure the test itself is correct. Then it runs the same assertions on a timeseries
 * collection (both with, and without, a 2dsphere index).
 *
 * @tags: [
 *     does_not_support_transactions,
 *     does_not_support_stepdowns,
 *     requires_fcv_51,
 *     requires_pipeline_optimization,
 *     requires_timeseries,
 *     # Explain of a resolved view must be executed by mongos.
 *     directly_against_shardsvrs_incompatible,
 * ]
 */

(function() {
"use strict";

load("jstests/libs/analyze_plan.js");
load("jstests/libs/feature_flag_util.js");

if (!FeatureFlagUtil.isEnabled(db, "TimeseriesMetricIndexes")) {
    jsTestLog(
        "Skipped test as the featureFlagTimeseriesMetricIndexes feature flag is not enabled.");
    return;
}
Random.setRandomSeed();

// Value is taken from geoconstants.h.
const earthRadiusMeters = (6378.1 * 1000);
const earthCircumferenceMeters = earthRadiusMeters * Math.PI * 2;
function degreesToMeters(degrees) {
    return degrees * (earthCircumferenceMeters / 360);
}

function insertTestData(coll) {
    // When these points are interpreted as spherical coordinates, [long, lat],
    // the units are interpreted as degrees.
    const nMeasurements = 10;
    const docs = [];
    for (let i = 0; i < nMeasurements; i++) {
        docs.push({
            time: ISODate(),
            loc: [0, i],
        });
    }
    // Include a few extra docs with no 'loc' field.
    // $geoNear queries should exclude these even when there is no minDistance/maxDistance.
    docs.push({time: ISODate(), abc: 123});
    docs.push({time: ISODate(), abc: 456});

    // Insert in a random order to ensure queries are really sorting.
    Array.shuffle(docs);
    assert.commandWorked(coll.insert(docs));
}

function runFlatExamples(coll, isTimeseries) {
    let pipeline, plan;

    // Make sure results are ordered by flat (planar) distance.
    // [180, 0] is closer to [0, 0] than [0, 9], in flat geometry.
    pipeline = [
        {
            $geoNear: {
                near: [180, 0],
                key: 'loc',
                distanceField: "distance",
            }
        },
        {$project: {_id: 0, loc: 1, distance: {$floor: "$distance"}}},
        {$limit: 1},
    ];
    assert.docEq(coll.aggregate(pipeline).toArray(), [
        {loc: [0, 0], distance: 180},
    ]);

    // For the rest of the examples, query from [0, 0] because it makes distances more convenient.

    // Test entire collection.
    pipeline = [
        {
            $geoNear: {
                near: [0, 0],
                key: 'loc',
                distanceField: "distance",
            }
        },
        {$project: {_id: 0, loc: 1, distance: 1}},
    ];
    assert.docEq(coll.aggregate(pipeline).toArray(), [
        {loc: [0, 0], distance: 0},
        {loc: [0, 1], distance: 1},
        {loc: [0, 2], distance: 2},
        {loc: [0, 3], distance: 3},
        {loc: [0, 4], distance: 4},
        {loc: [0, 5], distance: 5},
        {loc: [0, 6], distance: 6},
        {loc: [0, 7], distance: 7},
        {loc: [0, 8], distance: 8},
        {loc: [0, 9], distance: 9},
    ]);
    plan = coll.explain().aggregate(pipeline);
    // Since we don't support '2d' index on time-series metrics, and '2dsphere' indexes can't
    // answer flat queries, we always expect a collscan for timeseries.
    if (isTimeseries) {
        assert(aggPlanHasStage(plan, 'COLLSCAN'), plan);
    } else {
        assert(aggPlanHasStage(plan, 'GEO_NEAR_2D'), plan);
    }

    // Limit number of results with $limit.
    pipeline = [
        {
            $geoNear: {
                near: [0, 0],
                key: 'loc',
                distanceField: "distance",
            }
        },
        {$limit: 5},
        {$project: {_id: 0, loc: 1, distance: 1}},
    ];
    assert.docEq(coll.aggregate(pipeline).toArray(), [
        {loc: [0, 0], distance: 0},
        {loc: [0, 1], distance: 1},
        {loc: [0, 2], distance: 2},
        {loc: [0, 3], distance: 3},
        {loc: [0, 4], distance: 4},
    ]);
    plan = coll.explain().aggregate(pipeline);
    if (isTimeseries) {
        assert(aggPlanHasStage(plan, 'COLLSCAN'), plan);
    } else {
        assert(aggPlanHasStage(plan, 'GEO_NEAR_2D'), plan);
    }

    // Upper bound distance with maxDistance.
    pipeline = [
        {
            $geoNear: {
                near: [0, 0],
                key: 'loc',
                distanceField: "distance",
                maxDistance: 6,
            }
        },
        {$project: {_id: 0, loc: 1, distance: 1}},
    ];
    assert.docEq(coll.aggregate(pipeline).toArray(), [
        {loc: [0, 0], distance: 0},
        {loc: [0, 1], distance: 1},
        {loc: [0, 2], distance: 2},
        {loc: [0, 3], distance: 3},
        {loc: [0, 4], distance: 4},
        {loc: [0, 5], distance: 5},
        {loc: [0, 6], distance: 6},
    ]);
    plan = coll.explain().aggregate(pipeline);
    if (isTimeseries) {
        assert(aggPlanHasStage(plan, 'COLLSCAN'), plan);
    } else {
        assert(aggPlanHasStage(plan, 'GEO_NEAR_2D'), plan);
    }

    // Lower bound distance with minDistance.
    pipeline = [
        {
            $geoNear: {
                near: [0, 0],
                key: 'loc',
                distanceField: "distance",
                minDistance: 3,
            }
        },
        {$project: {_id: 0, loc: 1, distance: 1}},
    ];
    assert.docEq(coll.aggregate(pipeline).toArray(), [
        {loc: [0, 3], distance: 3},
        {loc: [0, 4], distance: 4},
        {loc: [0, 5], distance: 5},
        {loc: [0, 6], distance: 6},
        {loc: [0, 7], distance: 7},
        {loc: [0, 8], distance: 8},
        {loc: [0, 9], distance: 9},
    ]);
    plan = coll.explain().aggregate(pipeline);
    if (isTimeseries) {
        assert(aggPlanHasStage(plan, 'COLLSCAN'), plan);
    } else {
        assert(aggPlanHasStage(plan, 'GEO_NEAR_2D'), plan);
    }

    // Bound distance with both minDistance/maxDistance.
    pipeline = [
        {
            $geoNear: {
                near: [0, 0],
                key: 'loc',
                distanceField: "distance",
                minDistance: 3,
                maxDistance: 6,
            }
        },
        {$project: {_id: 0, loc: 1, distance: 1}},
    ];
    assert.docEq(coll.aggregate(pipeline).toArray(), [
        {loc: [0, 3], distance: 3},
        {loc: [0, 4], distance: 4},
        {loc: [0, 5], distance: 5},
        {loc: [0, 6], distance: 6},
    ]);
    plan = coll.explain().aggregate(pipeline);
    if (isTimeseries) {
        assert(aggPlanHasStage(plan, 'COLLSCAN'), plan);
    } else {
        assert(aggPlanHasStage(plan, 'GEO_NEAR_2D'), plan);
    }

    // Test interaction of distanceMultiplier with minDistance/maxDistance.
    pipeline = [
        {
            $geoNear: {
                near: [0, 0],
                key: 'loc',
                distanceField: "distance",
                distanceMultiplier: 10,
                minDistance: 3,
                maxDistance: 6,
            }
        },
        {$project: {_id: 0, loc: 1, distance: 1}},
    ];
    assert.docEq(coll.aggregate(pipeline).toArray(), [
        {loc: [0, 3], distance: 30},
        {loc: [0, 4], distance: 40},
        {loc: [0, 5], distance: 50},
        {loc: [0, 6], distance: 60},
    ]);
    plan = coll.explain().aggregate(pipeline);
    if (isTimeseries) {
        assert(aggPlanHasStage(plan, 'COLLSCAN'), plan);
    } else {
        assert(aggPlanHasStage(plan, 'GEO_NEAR_2D'), plan);
    }

    // 'query' option is not allowed on time-series collection.
    pipeline = [
        {
            $geoNear: {
                near: [0, 0],
                key: 'loc',
                distanceField: 'distance',
                query: {no_such_field: 123},
            }
        },
    ];
    if (isTimeseries) {
        assert.throwsWithCode(() => coll.aggregate(pipeline).toArray(), [
            // Must not specify 'query' for $geoNear on a time-series collection; use $match instead
            1938439,
            5860207,
        ]);
    } else {
        assert.docEq([], coll.aggregate(pipeline).toArray());
    }

    // 'includeLocs' is not implemented.
    pipeline = [
        {
            $geoNear: {
                near: [0, 0],
                key: 'loc',
                distanceField: 'distance',
                includeLocs: "abc",
            }
        },
    ];
    if (isTimeseries) {
        // $geoNear 'includeLocs' is not supported on time-series metrics
        assert.throwsWithCode(() => coll.aggregate(pipeline).toArray(), 5860208);
    } else {
        assert.gt(coll.aggregate(pipeline).toArray().length, 0);
    }
}

function runSphereExamples(coll, isTimeseries, has2dsphereIndex, scaleResult, query) {
    let pipeline, plan;

    // Test entire collection.
    pipeline = [
        {
            $geoNear: Object.assign({}, query, {
                key: 'loc',
                distanceField: "distance",
            })
        },
        {$project: {_id: 0, loc: 1, distance: {$floor: {$multiply: [scaleResult, "$distance"]}}}},
    ];
    assert.docEq(coll.aggregate(pipeline).toArray(), [
        {loc: [0, 9], distance: Math.floor(degreesToMeters(180 - 9))},
        {loc: [0, 8], distance: Math.floor(degreesToMeters(180 - 8))},
        {loc: [0, 7], distance: Math.floor(degreesToMeters(180 - 7))},
        {loc: [0, 6], distance: Math.floor(degreesToMeters(180 - 6))},
        {loc: [0, 5], distance: Math.floor(degreesToMeters(180 - 5))},
        {loc: [0, 4], distance: Math.floor(degreesToMeters(180 - 4))},
        {loc: [0, 3], distance: Math.floor(degreesToMeters(180 - 3))},
        {loc: [0, 2], distance: Math.floor(degreesToMeters(180 - 2))},
        {loc: [0, 1], distance: Math.floor(degreesToMeters(180 - 1))},
        {loc: [0, 0], distance: Math.floor(degreesToMeters(180 - 0))},
    ]);
    plan = coll.explain().aggregate(pipeline);
    if (isTimeseries) {
        // Without a maxDistance we have to unpack every bucket and sort the events.
        // However, we do include a $geoWithin predicate to filter out any non-geo documents.
        // This means we end up doing an index scan, which might or might not be beneficial
        // depending on how many buckets it allows us to exclude.
        if (has2dsphereIndex) {
            assert(aggPlanHasStage(plan, 'IXSCAN'), plan);
        } else {
            assert(aggPlanHasStage(plan, 'COLLSCAN'), plan);
        }
    } else {
        // We progressively scan larger and larger portions of the index.
        assert(aggPlanHasStage(plan, 'GEO_NEAR_2DSPHERE'), plan);
    }

    // Limit number of results with $limit.
    pipeline = [
        {
            $geoNear: Object.assign({}, query, {
                key: 'loc',
                distanceField: "distance",
            })
        },
        {$limit: 5},
        {$project: {_id: 0, loc: 1, distance: {$floor: {$multiply: [scaleResult, "$distance"]}}}},
    ];
    assert.docEq(coll.aggregate(pipeline).toArray(), [
        {loc: [0, 9], distance: Math.floor(degreesToMeters(180 - 9))},
        {loc: [0, 8], distance: Math.floor(degreesToMeters(180 - 8))},
        {loc: [0, 7], distance: Math.floor(degreesToMeters(180 - 7))},
        {loc: [0, 6], distance: Math.floor(degreesToMeters(180 - 6))},
        {loc: [0, 5], distance: Math.floor(degreesToMeters(180 - 5))},
    ]);
    plan = coll.explain().aggregate(pipeline);
    if (isTimeseries) {
        // Without a maxDistance we have to unpack every bucket and sort the events.
        // But, at least with a $limit stage we can do a top-k sort (although this doesn't
        // seem to show up in explain()).
        if (has2dsphereIndex) {
            assert(aggPlanHasStage(plan, 'IXSCAN'), plan);
        } else {
            assert(aggPlanHasStage(plan, 'COLLSCAN'), plan);
        }
    } else {
        assert(aggPlanHasStage(plan, 'GEO_NEAR_2DSPHERE'), plan);
    }

    // Upper bound distance with maxDistance.
    pipeline = [
        {
            $geoNear: Object.assign({}, query, {
                key: 'loc',
                distanceField: "distance",
                maxDistance: Math.ceil(degreesToMeters(180 - 3)) / scaleResult,
            })
        },
        {$project: {_id: 0, loc: 1, distance: {$floor: {$multiply: [scaleResult, "$distance"]}}}},
    ];
    assert.docEq(coll.aggregate(pipeline).toArray(), [
        {loc: [0, 9], distance: Math.floor(degreesToMeters(180 - 9))},
        {loc: [0, 8], distance: Math.floor(degreesToMeters(180 - 8))},
        {loc: [0, 7], distance: Math.floor(degreesToMeters(180 - 7))},
        {loc: [0, 6], distance: Math.floor(degreesToMeters(180 - 6))},
        {loc: [0, 5], distance: Math.floor(degreesToMeters(180 - 5))},
        {loc: [0, 4], distance: Math.floor(degreesToMeters(180 - 4))},
        {loc: [0, 3], distance: Math.floor(degreesToMeters(180 - 3))},
    ]);
    plan = coll.explain().aggregate(pipeline);
    if (isTimeseries) {
        // With maxDistance we can generate a $geoWithin predicate, which can use an index when
        // available.
        if (has2dsphereIndex) {
            assert(aggPlanHasStage(plan, 'IXSCAN'), plan);
        } else {
            assert(aggPlanHasStage(plan, 'COLLSCAN'), plan);
        }
    } else {
        assert(aggPlanHasStage(plan, 'GEO_NEAR_2DSPHERE'), plan);
    }

    // Lower bound distance with minDistance.
    pipeline = [
        {
            $geoNear: Object.assign({}, query, {
                key: 'loc',
                distanceField: "distance",
                minDistance: Math.floor(degreesToMeters(180 - 7)) / scaleResult,
            })
        },
        {$project: {_id: 0, loc: 1, distance: {$floor: {$multiply: [scaleResult, "$distance"]}}}},
    ];
    assert.docEq(coll.aggregate(pipeline).toArray(), [
        {loc: [0, 7], distance: Math.floor(degreesToMeters(180 - 7))},
        {loc: [0, 6], distance: Math.floor(degreesToMeters(180 - 6))},
        {loc: [0, 5], distance: Math.floor(degreesToMeters(180 - 5))},
        {loc: [0, 4], distance: Math.floor(degreesToMeters(180 - 4))},
        {loc: [0, 3], distance: Math.floor(degreesToMeters(180 - 3))},
        {loc: [0, 2], distance: Math.floor(degreesToMeters(180 - 2))},
        {loc: [0, 1], distance: Math.floor(degreesToMeters(180 - 1))},
        {loc: [0, 0], distance: Math.floor(degreesToMeters(180 - 0))},
    ]);
    plan = coll.explain().aggregate(pipeline);
    if (isTimeseries) {
        if (has2dsphereIndex) {
            assert(aggPlanHasStage(plan, 'IXSCAN'), plan);
        } else {
            assert(aggPlanHasStage(plan, 'COLLSCAN'), plan);
        }
    } else {
        assert(aggPlanHasStage(plan, 'GEO_NEAR_2DSPHERE'), plan);
    }

    // Bound distance with both minDistance/maxDistance.
    pipeline = [
        {
            $geoNear: Object.assign({}, query, {
                key: 'loc',
                distanceField: "distance",
                minDistance: Math.floor(degreesToMeters(180 - 7)) / scaleResult,
                maxDistance: Math.ceil(degreesToMeters(180 - 3)) / scaleResult,
            })
        },
        {$project: {_id: 0, loc: 1, distance: {$floor: {$multiply: [scaleResult, "$distance"]}}}},
    ];
    assert.docEq(coll.aggregate(pipeline).toArray(), [
        {loc: [0, 7], distance: Math.floor(degreesToMeters(180 - 7))},
        {loc: [0, 6], distance: Math.floor(degreesToMeters(180 - 6))},
        {loc: [0, 5], distance: Math.floor(degreesToMeters(180 - 5))},
        {loc: [0, 4], distance: Math.floor(degreesToMeters(180 - 4))},
        {loc: [0, 3], distance: Math.floor(degreesToMeters(180 - 3))},
    ]);
    plan = coll.explain().aggregate(pipeline);
    if (isTimeseries) {
        if (has2dsphereIndex) {
            assert(aggPlanHasStage(plan, 'IXSCAN'), plan);
        } else {
            assert(aggPlanHasStage(plan, 'COLLSCAN'), plan);
        }
    } else {
        assert(aggPlanHasStage(plan, 'GEO_NEAR_2DSPHERE'), plan);
    }

    // Test interaction of distanceMultiplier with minDistance/maxDistance.
    pipeline = [
        {
            $geoNear: Object.assign({}, query, {
                key: 'loc',
                distanceField: "distance",
                distanceMultiplier: 10,
                minDistance: Math.floor(degreesToMeters(180 - 7)) / scaleResult,
                maxDistance: Math.ceil(degreesToMeters(180 - 3)) / scaleResult,
            })
        },
        {$project: {_id: 0, loc: 1, distance: {$floor: {$multiply: [scaleResult, "$distance"]}}}},
    ];
    assert.docEq(coll.aggregate(pipeline).toArray(), [
        {loc: [0, 7], distance: Math.floor(10 * degreesToMeters(180 - 7))},
        {loc: [0, 6], distance: Math.floor(10 * degreesToMeters(180 - 6))},
        {loc: [0, 5], distance: Math.floor(10 * degreesToMeters(180 - 5))},
        {loc: [0, 4], distance: Math.floor(10 * degreesToMeters(180 - 4))},
        {loc: [0, 3], distance: Math.floor(10 * degreesToMeters(180 - 3))},
    ]);
    plan = coll.explain().aggregate(pipeline);
    if (isTimeseries) {
        if (has2dsphereIndex) {
            assert(aggPlanHasStage(plan, 'IXSCAN'), plan);
        } else {
            assert(aggPlanHasStage(plan, 'COLLSCAN'), plan);
        }
    } else {
        assert(aggPlanHasStage(plan, 'GEO_NEAR_2DSPHERE'), plan);
    }

    // 'query' option is not allowed on time-series collections.
    pipeline = [
        {
            $geoNear: Object.assign({}, query, {
                key: 'loc',
                distanceField: 'distance',
                query: {no_such_field: 123},
            })
        },
    ];
    if (isTimeseries) {
        assert.throwsWithCode(() => coll.aggregate(pipeline).toArray(), [
            // Must not specify 'query' for $geoNear on a time-series collection; use $match instead
            1938439,
            5860207,
        ]);
    } else {
        assert.docEq([], coll.aggregate(pipeline).toArray());
    }
    // Instead use $match.
    pipeline = [
        {
            $geoNear: Object.assign({}, query, {
                key: 'loc',
                distanceField: 'distance',
            })
        },
        {$match: {no_such_field: 456}},
    ];
    assert.docEq(coll.aggregate(pipeline).toArray(), []);
    plan = coll.explain().aggregate(pipeline);
    if (isTimeseries) {
        if (has2dsphereIndex) {
            assert(aggPlanHasStage(plan, 'IXSCAN'), plan);
        } else {
            assert(aggPlanHasStage(plan, 'COLLSCAN'), plan);
        }
        // The additional $match predicate should be pushed down and combined with the geo
        // predicate. That is, the initial $cursor stage should include both.
        const cursorStage = getAggPlanStage(plan, '$cursor');
        const parsedQuery = cursorStage.$cursor.queryPlanner.parsedQuery;
        const parsedQueryString = tojson(parsedQuery);
        const planString = tojson(plan);
        assert.includes(parsedQueryString, 'no_such_field', planString);
        assert.includes(parsedQueryString, '_internalBucketGeoWithin', planString);
    } else {
        assert(aggPlanHasStage(plan, 'GEO_NEAR_2DSPHERE'), plan);
    }

    // 'includeLocs' is not implemented.
    pipeline = [
        {
            $geoNear: Object.assign({}, query, {
                key: 'loc',
                distanceField: 'distance',
                includeLocs: "abc",
            })
        },
    ];
    if (isTimeseries) {
        // $geoNear 'includeLocs' is not supported on time-series metrics
        assert.throwsWithCode(() => coll.aggregate(pipeline).toArray(), 5860208);
    } else {
        assert.gt(coll.aggregate(pipeline).toArray().length, 0);
    }
}

function runExamples(coll, isTimeseries, has2dsphereIndex) {
    runFlatExamples(coll, isTimeseries, has2dsphereIndex);

    // There are three different ways to specify a spherical query.

    // GeoJSON, implicitly uses spherical geometry.
    runSphereExamples(
        coll, isTimeseries, has2dsphereIndex, 1, {near: {type: "Point", coordinates: [180, 0]}});

    // GeoJSON, with explicit spherical: true.
    runSphereExamples(coll, isTimeseries, has2dsphereIndex, 1, {
        near: {type: "Point", coordinates: [180, 0]},
        spherical: true,
    });

    // [x, y] point, but with explicit spherical: true.
    // In this case, the resulting distances are expressed in radians by default, so we scale up
    // the query results by 'earthRadiusMeters' before comparing with the expectedresult.
    // We also scale down the maxDistance/minDistance bounds.
    runSphereExamples(coll, isTimeseries, has2dsphereIndex, earthRadiusMeters, {
        near: [180, 0],
        spherical: true,
    });
}

// Test $geoNear query results in several contexts:
// 1. on a normal collection, with a 2dsphere index
// 2. on a timeseries collection, with no index
// 3. on a timeseries collection, with a 2dsphere index on measurements

// 1. Test a normal collection with a 2dsphere index.
//    This is our baseline that ensures runExamples() is making correct assertions.
{
    const coll = db.getCollection("timeseries_geonear_measurements_baseline");
    coll.drop();
    assert.commandWorked(coll.createIndex({loc: '2dsphere'}));

    // Actually, we also need a '2d' index for the flat examples to succeed.
    assert.commandWorked(coll.createIndex({loc: '2d'}));

    insertTestData(coll);
    runExamples(coll, false /* isTimeseries */, true /* has2dsphereIndex */);
}

// 2. Test a timeseries collection, with no index.
//    This ensures that the rewrite of $geoNear to $geoWithin + $sort is correct.
//    But it's a naive / unrealistic query plan.
{
    const coll = db.getCollection("timeseries_geonear_measurements_noindex");
    coll.drop();
    assert.commandWorked(db.createCollection(coll.getName(), {timeseries: {timeField: 'time'}}));

    insertTestData(coll);
    runExamples(coll, true /* isTimeseries */, false /* has2dsphereIndex */);
}

// 3. Test a timeseries collection, with a 2dsphere index on measurements.
//    This should work if $geoWithin is indexed correctly.
{
    const coll = db.getCollection("timeseries_geonear_measurements_indexed");
    coll.drop();
    assert.commandWorked(db.createCollection(coll.getName(), {timeseries: {timeField: 'time'}}));
    assert.commandWorked(coll.createIndex({loc: '2dsphere'}));

    // Make sure the 2dsphere index exists. (If the collection is implicitly sharded then we will
    // also see an implicitly created index.)
    const buckets = db.getCollection('system.buckets.' + coll.getName());
    assert.sameMembers(buckets.getIndexKeys(),
                       FixtureHelpers.isSharded(buckets)
                           ? [{'data.loc': '2dsphere_bucket'}, {'control.min.time': 1}]
                           : [{'data.loc': '2dsphere_bucket'}]);

    insertTestData(coll);
    runExamples(coll, true /* isTimeseries */, true /* has2dsphereIndex */);
}
})();
