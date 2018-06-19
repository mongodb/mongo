// Test $minDistance option for $near and $nearSphere queries, and the $geoNear aggregation stage.
// @tags: [requires_fastcount, requires_getmore]

(function() {
    "use strict";

    load("jstests/libs/geo_math.js");

    var t = db.geo_mindistance;
    t.drop();

    const km = 1000;

    /**
     * Asserts that two numeric values are equal within some absolute error.
     */
    function assertApproxEqual(rhs, lhs, error, msg) {
        assert.lt(Math.abs(rhs - rhs), error, msg);
    }

    /**
     * Count documents within some radius of (0, 0), in kilometers. With this function we can use
     * the existing $maxDistance option to test the newer $minDistance option's behavior.
     */
    function n_docs_within(radius_km) {
        // $geoNear's distances are in meters for geoJSON points.
        return t
            .aggregate([
                {
                  $geoNear: {
                      near: {type: 'Point', coordinates: [0, 0]},
                      distanceField: "dis",
                      spherical: true,
                      maxDistance: radius_km * km,
                  }
                },
                {$limit: 1000}
            ])
            .itcount();
    }

    //
    // Setup.
    //

    /**
     * Make 121 points from long, lat = (0, 0) (in Gulf of Guinea) to (10, 10) (inland Nigeria).
     */
    for (var x = 0; x <= 10; x += 1) {
        for (var y = 0; y <= 10; y += 1) {
            t.insert({loc: [x, y]});
        }
    }

    t.ensureIndex({loc: "2dsphere"});

    var n_docs = t.count(), geoJSONPoint = {type: 'Point', coordinates: [0, 0]},
        legacyPoint = [0, 0];

    //
    // Test $near with GeoJSON point (required for $near with 2dsphere index). min/maxDistance are
    // in meters.
    //

    var n_min1400_count =
        t.find({loc: {$near: {$geometry: geoJSONPoint, $minDistance: 1400 * km}}}).count();

    assert.eq(n_docs - n_docs_within(1400),
              n_min1400_count,
              "Expected " + (n_docs - n_docs_within(1400)) +
                  " points $near (0, 0) with $minDistance 1400 km, got " + n_min1400_count);

    var n_bw500_and_1000_count =
        t.find({
             loc: {
                 $near:
                     {$geometry: geoJSONPoint, $minDistance: 500 * km, $maxDistance: 1000 * km}
             }
         }).count();

    assert.eq(n_docs_within(1000) - n_docs_within(500),
              n_bw500_and_1000_count,
              "Expected " + (n_docs_within(1000) - n_docs_within(500)) +
                  " points $near (0, 0) with $minDistance 500 km and $maxDistance 1000 km, got " +
                  n_bw500_and_1000_count);

    //
    // $nearSphere with 2dsphere index can take a legacy or GeoJSON point. First test $nearSphere
    // with legacy point. min/maxDistance are in radians.
    //

    n_min1400_count =
        t.find({loc: {$nearSphere: legacyPoint, $minDistance: metersToRadians(1400 * km)}}).count();

    assert.eq(n_docs - n_docs_within(1400),
              n_min1400_count,
              "Expected " + (n_docs - n_docs_within(1400)) +
                  " points $nearSphere (0, 0) with $minDistance 1400 km, got " + n_min1400_count);

    n_bw500_and_1000_count = t.find({
                                  loc: {
                                      $nearSphere: legacyPoint,
                                      $minDistance: metersToRadians(500 * km),
                                      $maxDistance: metersToRadians(1000 * km)
                                  }
                              }).count();

    assert.eq(
        n_docs_within(1000) - n_docs_within(500),
        n_bw500_and_1000_count,
        "Expected " + (n_docs_within(1000) - n_docs_within(500)) +
            " points $nearSphere (0, 0) with $minDistance 500 km and $maxDistance 1000 km, got " +
            n_bw500_and_1000_count);

    //
    // Test $nearSphere with GeoJSON point. min/maxDistance are in meters.
    //

    n_min1400_count = t.find({loc: {$nearSphere: geoJSONPoint, $minDistance: 1400 * km}}).count();

    assert.eq(n_docs - n_docs_within(1400),
              n_min1400_count,
              "Expected " + (n_docs - n_docs_within(1400)) +
                  " points $nearSphere (0, 0) with $minDistance 1400 km, got " + n_min1400_count);

    n_bw500_and_1000_count =
        t.find({
             loc: {$nearSphere: geoJSONPoint, $minDistance: 500 * km, $maxDistance: 1000 * km}
         }).count();

    assert.eq(
        n_docs_within(1000) - n_docs_within(500),
        n_bw500_and_1000_count,
        "Expected " + (n_docs_within(1000) - n_docs_within(500)) +
            " points $nearSphere (0, 0) with $minDistance 500 km and $maxDistance 1000 km, got " +
            n_bw500_and_1000_count);

    //
    // Test $geoNear aggregation stage with GeoJSON point. Distances are in meters.
    //

    let geoNearCount = t.aggregate({
                            $geoNear: {
                                near: {type: 'Point', coordinates: [0, 0]},
                                minDistance: 1400 * km,
                                spherical: true,
                                distanceField: "d",
                            }
                        }).itcount();
    assert.eq(n_docs - n_docs_within(1400),
              geoNearCount,
              "Expected " + (n_docs - n_docs_within(1400)) +
                  " points geoNear (0, 0) with $minDistance 1400 km, got " + geoNearCount);

    geoNearCount = t.aggregate({
                        $geoNear: {
                            near: {type: 'Point', coordinates: [0, 0]},
                            minDistance: 500 * km,
                            maxDistance: 1000 * km,
                            spherical: true,
                            distanceField: "d",
                        }
                    }).itcount();
    assert.eq(n_docs_within(1000) - n_docs_within(500),
              geoNearCount,
              "Expected " + (n_docs_within(1000) - n_docs_within(500)) +
                  " points geoNear (0, 0) with $minDistance 500 km and $maxDistance 1000 km, got " +
                  geoNearCount);

    //
    // Test $geoNear aggregation stage with legacy point. Distances are in radians.
    //

    geoNearCount = t.aggregate({
                        $geoNear: {
                            near: legacyPoint,
                            minDistance: metersToRadians(1400 * km),
                            spherical: true,
                            distanceField: "d",
                        }
                    }).itcount();
    assert.eq(n_docs - n_docs_within(1400),
              geoNearCount,
              "Expected " + (n_docs - n_docs_within(1400)) +
                  " points geoNear (0, 0) with $minDistance 1400 km, got " + geoNearCount);

    geoNearCount = t.aggregate({
                        $geoNear: {
                            near: legacyPoint,
                            minDistance: metersToRadians(500 * km),
                            maxDistance: metersToRadians(1000 * km),
                            spherical: true,
                            distanceField: "d",
                        }
                    }).itcount();
    assert.eq(n_docs_within(1000) - n_docs_within(500),
              geoNearCount,
              "Expected " + (n_docs_within(1000) - n_docs_within(500)) +
                  " points geoNear (0, 0) with $minDistance 500 km and $maxDistance 1000 km, got " +
                  geoNearCount);

    t.drop();
    assert.commandWorked(t.createIndex({loc: "2d"}));
    assert.writeOK(t.insert({loc: [0, 40]}));
    assert.writeOK(t.insert({loc: [0, 41]}));
    assert.writeOK(t.insert({loc: [0, 42]}));

    // Test minDistance for 2d index with $near.
    assert.eq(3, t.find({loc: {$near: [0, 0]}}).itcount());
    assert.eq(1, t.find({loc: {$near: [0, 0], $minDistance: 41.5}}).itcount());

    // Test minDistance for 2d index with $nearSphere. Distances are in radians.
    assert.eq(3, t.find({loc: {$nearSphere: [0, 0]}}).itcount());
    assert.eq(1, t.find({loc: {$nearSphere: [0, 0], $minDistance: deg2rad(41.5)}}).itcount());

    // Test minDistance for 2d index with $geoNear stage and spherical=false.
    let cmdResult =
        t.aggregate({$geoNear: {near: [0, 0], spherical: false, distanceField: "dis"}}).toArray();
    assert.eq(3, cmdResult.length);
    assert.eq(40, cmdResult[0].dis);
    assert.eq(41, cmdResult[1].dis);
    assert.eq(42, cmdResult[2].dis);

    cmdResult = t.aggregate({
                     $geoNear: {
                         near: [0, 0],
                         minDistance: 41.5,
                         spherical: false,
                         distanceField: "dis",
                     }
                 }).toArray();
    assert.eq(1, cmdResult.length);
    assert.eq(42, cmdResult[0].dis);

    // Test minDistance for 2d index with $geoNear stage and spherical=true. Distances are in
    // radians.
    cmdResult =
        t.aggregate({$geoNear: {near: [0, 0], spherical: true, distanceField: "dis"}}).toArray();
    assert.eq(3, cmdResult.length);
    assertApproxEqual(deg2rad(40), cmdResult[0].dis, 1e-3);
    assertApproxEqual(deg2rad(41), cmdResult[1].dis, 1e-3);
    assertApproxEqual(deg2rad(42), cmdResult[2].dis, 1e-3);

    cmdResult = t.aggregate({
                     $geoNear: {
                         near: [0, 0],
                         minDistance: deg2rad(41.5),
                         spherical: true,
                         distanceField: "dis",
                     }
                 }).toArray();
    assert.eq(1, cmdResult.length);
    assertApproxEqual(deg2rad(42), cmdResult[0].dis, 1e-3);
}());
