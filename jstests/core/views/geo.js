/**
 * Tests that $near and $nearSphere work on an identity view. This is a prerequisite for
 * supporting $near and $nearSphere on time-series views.
 *
 * @tags [
 *   requires_fcv_51,
 * ]
 */
(function() {
"use strict";

load('jstests/libs/analyze_plan.js');
load("jstests/core/timeseries/libs/timeseries.js");

if (!TimeseriesTest.timeseriesMetricIndexesEnabled(db.getMongo())) {
    jsTestLog("Skipping test because the time-series collection feature flag is disabled");
    return;
}

// Sharding passthrough suites override getCollection() implicitly shard the collection.
// This fails when the view already exists (from a previous run of this test), because you can't
// shard a view. To work around this, drop the view before we even call getCollection().
const collName = 'views_geo_collection';
const viewName = 'views_geo_view';
// Sharding passthrough suites also override the drop() method, so use runCommand() to bypass it.
assert.commandWorkedOrFailedWithCode(db.runCommand({'drop': collName}),
                                     [ErrorCodes.NamespaceNotFound]);
assert.commandWorkedOrFailedWithCode(db.runCommand({'drop': viewName}),
                                     [ErrorCodes.NamespaceNotFound]);
// Allow the collection to be implicitly sharded.
const coll = db.getCollection(collName);
// Allow the view to be implicitly sharded.
const view = db.getCollection(viewName);
// Drop the view again so we can create it, as a view.
assert.commandWorkedOrFailedWithCode(db.runCommand({'drop': viewName}),
                                     [ErrorCodes.NamespaceNotFound]);

// Create some documents with 2D coordinates.
// The coordinates are [0, 0] ... [0, 90]. On a plane, they lie on the positive y axis,
// while on a sphere they lie on the prime meridian, from the equator to the north pole.
const numDocs = 11;
const step = 90 / (numDocs - 1);
coll.drop();
coll.insert(Array.from({length: numDocs}, (_, i) => ({_id: i, loc: [0, step * i]})));
assert.eq(numDocs, coll.aggregate([]).itcount());
// Make sure the last doc lands exactly on the north pole.
assert.eq(coll.find().sort({_id: -1}).limit(1).next(), {_id: numDocs - 1, loc: [0, 90]});
// Define an identity view.
db.createView(view.getName(), coll.getName(), []);
// Make sure the view contents match the collection.
assert.eq(numDocs, view.aggregate([]).itcount());
assert.sameMembers(coll.aggregate([]).toArray(), view.aggregate([]).toArray());

// There are several choices in how the query is written:
// - $near or $nearSphere
// - [x, y] or GeoJSON
// - minDistance specified or not
// - maxDistance specified or not
// And there are many possible outcomes:
// - Error, or not.
// - Points ordered by spherical distance, or planar.
// - minDistance/maxDistance interpreted as which units:
//   - degrees?
//   - radians?
//   - meters?
// The outcome may depend on which indexes are present.

// Sets up and tears down indexes around a given function.
function withIndexes(indexKeys, func) {
    if (indexKeys.length) {
        assert.commandWorked(coll.createIndexes(indexKeys));
    }
    try {
        return func();
    } finally {
        for (const key of indexKeys) {
            assert.commandWorked(coll.dropIndex(key));
        }
    }
}

// Runs the cursor and asserts that it fails because a required geospatial index was missing.
function assertNoIndex(cursor) {
    const err = assert.throws(() => cursor.next());
    assert.contains(err.code, [ErrorCodes.NoQueryExecutionPlans, ErrorCodes.IndexNotFound], err);
    assert(err.message.includes("$geoNear requires a 2d or 2dsphere index, but none were found") ||
               err.message.includes("unable to find index for $geoNear query"),
           err);
    return err;
}

// Runs the cursor and returns an array of just the Y coordinate for each doc.
function getY(cursor) {
    return cursor.toArray().map(doc => doc.loc[1]);
}

// Runs the cursor and asserts that all the points appear, with those closer to [0, 0] first.
function assertOriginFirst(cursor) {
    assert.docEq(getY(cursor),
                 [0, 9, 18, 27, 36, 45, 54, 63, 72, 81, 90],
                 'Expected points closer to [0, 0] to be first.');
}

// Runs the cursor and asserts that all the points appear, with those closer to [0, 90] first.
function assertOriginLast(cursor) {
    assert.docEq(getY(cursor),
                 [90, 81, 72, 63, 54, 45, 36, 27, 18, 9, 0],
                 'Expected points closer to [0, 0] to be last.');
}

function degToRad(deg) {
    // 180 degrees = pi radians
    return deg * (Math.PI / 180);
}
function degToMeters(deg) {
    // Earth's circumference is roughly 40,000 km.
    // 360 degrees = 1 circumference
    const earthCircumference = 40074784;
    return deg * (earthCircumference / 360);
}

// Abbreviation for creating a GeoJSON point.
function geoJSON(coordinates) {
    return {type: "Point", coordinates};
}

// Test how $near/$nearSphere is interpreted: $nearSphere always means a spherical-distance query,
// but $near can mean either planar or spherical depending on how the query point is written.
// Also test which combinations are allowed with which indexes.
withIndexes([], () => {
    // With no geospatial indexes, $near and $nearSphere should always fail.
    assertNoIndex(coll.find({loc: {$near: [0, 0]}}));
    assertNoIndex(coll.find({loc: {$near: {$geometry: geoJSON([0, 0])}}}));
    assertNoIndex(coll.find({loc: {$nearSphere: [0, 0]}}));
    assertNoIndex(coll.find({loc: {$nearSphere: {$geometry: geoJSON([0, 0])}}}));

    assertNoIndex(view.find({loc: {$near: [0, 0]}}));
    assertNoIndex(view.find({loc: {$near: {$geometry: geoJSON([0, 0])}}}));
    assertNoIndex(view.find({loc: {$nearSphere: [0, 0]}}));
    assertNoIndex(view.find({loc: {$nearSphere: {$geometry: geoJSON([0, 0])}}}));
});
withIndexes([{loc: '2d'}], () => {
    // Queries written as GeoJSON mean spherical distance, so they fail without a '2dsphere' index.
    assertNoIndex(coll.find({loc: {$near: {$geometry: geoJSON([180, 0])}}}));
    assertNoIndex(view.find({loc: {$near: {$geometry: geoJSON([180, 0])}}}));

    assertNoIndex(coll.find({loc: {$nearSphere: {$geometry: geoJSON([180, 0])}}}));
    assertNoIndex(view.find({loc: {$nearSphere: {$geometry: geoJSON([180, 0])}}}));

    // $near [x, y] means planar distance, so it succeeds.
    assertOriginFirst(coll.find({loc: {$near: [180, 0]}}));
    assertOriginFirst(view.find({loc: {$near: [180, 0]}}));

    // Surprisingly, $nearSphere can use a '2d' index to do a spherical-distance query.
    // Also surprisingly, this only works with the [x, y] query syntax, not with GeoJSON.
    assertOriginLast(coll.find({loc: {$nearSphere: [180, 0]}}));
    assertOriginLast(view.find({loc: {$nearSphere: [180, 0]}}));
});
withIndexes([{loc: '2dsphere'}], () => {
    // When '2dsphere' is available but not '2d', we can only satisfy spherical-distance queries.

    // $near [x, y] fails because it means planar distance.
    assertNoIndex(coll.find({loc: {$near: [180, 0]}}));
    assertNoIndex(view.find({loc: {$near: [180, 0]}}));

    // $near with GeoJSON means spherical distance, so it succeeds.
    assertOriginLast(coll.find({loc: {$near: {$geometry: geoJSON([180, 0])}}}));
    assertOriginLast(view.find({loc: {$near: {$geometry: geoJSON([180, 0])}}}));

    // $nearSphere succeeds with either syntax.
    assertOriginLast(coll.find({loc: {$nearSphere: [180, 0]}}));
    assertOriginLast(view.find({loc: {$nearSphere: [180, 0]}}));

    assertOriginLast(coll.find({loc: {$nearSphere: {$geometry: geoJSON([180, 0])}}}));
    assertOriginLast(view.find({loc: {$nearSphere: {$geometry: geoJSON([180, 0])}}}));
});

// For the rest of the tests, both index types are available.
// With both types of indexes available, all the queries should succeed.
coll.createIndexes([{loc: '2d'}, {loc: '2dsphere'}]);

// $near [x, y] means planar.
assertOriginFirst(coll.find({loc: {$near: [180, 0]}}));
// GeoJSON and/or $nearSphere means spherical.
assertOriginLast(coll.find({loc: {$near: {$geometry: geoJSON([180, 0])}}}));
assertOriginLast(coll.find({loc: {$nearSphere: [180, 0]}}));
assertOriginLast(coll.find({loc: {$nearSphere: {$geometry: geoJSON([180, 0])}}}));

// $near [x, y] means planar distance.
assertOriginFirst(view.find({loc: {$near: [180, 0]}}));
// GeoJSON and/or $nearSphere means spherical.
assertOriginLast(view.find({loc: {$near: {$geometry: geoJSON([180, 0])}}}));
assertOriginLast(view.find({loc: {$nearSphere: [180, 0]}}));
assertOriginLast(view.find({loc: {$nearSphere: {$geometry: geoJSON([180, 0])}}}));

// Test how minDistance / maxDistance are interpreted.
{
    // In a planar-distance query, min/maxDistance make sense even without thinking about units
    // (degrees vs radians). You can think of the min/maxDistance as being the same units as the
    // [x, y] coordinates in the collection: pixels, miles; it doesn't matter.
    assert.eq(getY(coll.find({loc: {$near: [0, 0], $minDistance: 9, $maxDistance: 36}})),
              [9, 18, 27, 36]);
    assert.eq(getY(view.find({loc: {$near: [0, 0], $minDistance: 9, $maxDistance: 36}})),
              [9, 18, 27, 36]);

    // In a spherical-distance query, units do matter. The points in the collection are interpreted
    // as degrees [longitude, latitude].

    // Queries written as GeoJSON use meters.
    assert.eq(getY(coll.find({
                  loc: {
                      $near: {
                          $geometry: geoJSON([0, 0]),
                          $minDistance: degToMeters(9),
                          $maxDistance: degToMeters(36) + 1,
                      }
                  }
              })),
              [9, 18, 27, 36]);
    assert.eq(getY(coll.find({
                  loc: {
                      $nearSphere: {
                          $geometry: geoJSON([0, 0]),
                          $minDistance: degToMeters(9),
                          $maxDistance: degToMeters(36) + 1,
                      }
                  }
              })),
              [9, 18, 27, 36]);
    assert.eq(getY(view.find({
                  loc: {
                      $near: {
                          $geometry: geoJSON([0, 0]),
                          $minDistance: degToMeters(9),
                          $maxDistance: degToMeters(36) + 1,
                      }
                  }
              })),
              [9, 18, 27, 36]);
    assert.eq(getY(view.find({
                  loc: {
                      $nearSphere: {
                          $geometry: geoJSON([0, 0]),
                          $minDistance: degToMeters(9),
                          $maxDistance: degToMeters(36) + 1,
                      }
                  }
              })),
              [9, 18, 27, 36]);

    // $nearSphere [x, y] uses radians.
    assert.eq(
        getY(coll.find(
            {loc: {$nearSphere: [0, 0], $minDistance: degToRad(9), $maxDistance: degToRad(36)}})),
        [9, 18, 27, 36]);
    assert.eq(
        getY(view.find(
            {loc: {$nearSphere: [0, 0], $minDistance: degToRad(9), $maxDistance: degToRad(36)}})),
        [9, 18, 27, 36]);
}

// Test a few more odd examples: just confirm that the view and collection behave the same way.

function example(predicate, limit = 0) {
    // .find().limit(0) is interpreted as no limit.
    assert.eq(coll.find(predicate).limit(limit).itcount(),
              view.find(predicate).limit(limit).itcount(),
              predicate);
    assert.docEq(coll.find(predicate).limit(limit).toArray(),
                 view.find(predicate).limit(limit).toArray(),
                 predicate);
}

// We want the order of results to be predictable: no ties. The documents all have integer
// coordinates, so if we avoid nice numbers like .0 or .5 we know every document has a different
// distance from the query point.
example({loc: {$near: [0, 2.2]}});
example({loc: {$near: [0, 9.1]}});

// $near can be combined with another predicate.
example({loc: {$near: [0, 2.2]}, _id: {$ne: 2}});

// $near can have $minDistance and/or $maxDistance.
example({loc: {$near: [0, 30.1], $minDistance: 10}});
example({loc: {$near: [0, 30.1], $maxDistance: 20}});
example({loc: {$near: [0, 30.1], $minDistance: 10, $maxDistance: 20}});

// $near can have a limit.
example({loc: {$near: [0, 2.2]}}, 10);
example({loc: {$near: [0, 9.1]}}, 30);

// Try combining several options:
// - another predicate
// - min/max distance
// - limit
example({loc: {$near: [0, 30.1], $minDistance: 10, $maxDistance: 30}, _id: {$ne: 2}}, 20);

// .sort() throws away the $near order.
assert.docEq(coll.find({loc: {$near: [0, 100]}}).sort({'loc.1': 1}).limit(5).toArray(),
             view.find({loc: {$near: [0, 100]}}).sort({'loc.1': 1}).limit(5).toArray());

// Test wrapping: insert a point at -179 degrees, and query it at +179.
// It should be within about 2 degrees.
const wrapTestDoc = {
    _id: "wrap",
    loc: [-179, 0]
};
assert.commandWorked(coll.insert(wrapTestDoc));

// Test all 3 syntaxes on the collection, and on the view.
// These are all just alternative ways to specify the same spherical query.
// Although the results are consistent, the plan is slightly different when running on the view,
// because it runs as an aggregation.

const assertNearSphereQuery = explain => {
    assert(isQueryPlan(explain), explain);
    assert(planHasStage(db, explain, 'GEO_NEAR_2DSPHERE'), explain);
};
const assertNearSphereAgg = explain => {
    assert(isAggregationPlan(explain), explain);
    let stages = getAggPlanStages(explain, '$geoNearCursor');
    assert.neq(stages, [], explain);
};

let query = {
    loc: {
        $nearSphere: {
            $geometry: geoJSON([+179, 0]),
            $minDistance: degToMeters(1.9),
            $maxDistance: degToMeters(2.1)
        }
    }
};
assert.docEq(coll.find(query).toArray(), [wrapTestDoc]);
assert.docEq(view.find(query).toArray(), [wrapTestDoc]);
assertNearSphereQuery(coll.find(query).explain());
assertNearSphereAgg(view.find(query).explain());

query = {
    loc: {
        $near: {
            $geometry: geoJSON([+179, 0]),
            $minDistance: degToMeters(1.9),
            $maxDistance: degToMeters(2.1)
        }
    }
};
assert.docEq(coll.find(query).toArray(), [wrapTestDoc]);
assert.docEq(view.find(query).toArray(), [wrapTestDoc]);
assertNearSphereQuery(coll.find(query).explain());
assertNearSphereAgg(view.find(query).explain());

query = {
    loc: {$nearSphere: [+179, 0], $minDistance: degToRad(1.9), $maxDistance: degToRad(2.1)}
};
assert.docEq(coll.find(query).toArray(), [wrapTestDoc]);
assert.docEq(view.find(query).toArray(), [wrapTestDoc]);
assertNearSphereQuery(coll.find(query).explain());
assertNearSphereAgg(view.find(query).explain());
})();