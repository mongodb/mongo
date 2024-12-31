/*
 * Tests the behavior of the $geoNear stage without setting a distanceField.
 * @tags: [
 *   # $geoNear is not allowed in a facet.
 *   do_not_wrap_aggregations_in_facets,
 * ]
 */

const collName = jsTest.name();
const coll = db[collName];

// Test points that are exactly at the "near" point, close to the point, and far from the point.
// Distances are purposely chosen to be small so that distances in meters and radians are close.
const origin = {
    pt: [0, 0]
};
const near = {
    pt: [0.23, -0.32]
};
const far = {
    pt: [5.9, 0.0]
};
const furthest = {
    pt: [7.2, -1.4]
};

["2d", "2dsphere"].forEach(geoType => {
    jsTestLog(`Testing $geoNear with index {pt: "${geoType}"}`);
    coll.drop();

    // Create the desired index type and populate the collection.
    assert.commandWorked(coll.createIndex({pt: geoType}));
    [origin, near, far, furthest].forEach(doc => {
        doc.distFromOrigin = (geoType === "2dsphere") ? Geo.sphereDistance(doc.pt, origin.pt)
                                                      : Geo.distance(doc.pt, origin.pt);
        assert.commandWorked(coll.insert(doc));
    });

    /**
     * Helper function that runs a $geoNear aggregation near the origin, omitting distanceField, and
     * asserting that the results match 'expected'.
     */
    function assertGeoNearResults(point, expected) {
        const projStage = {$project: {_id: 0}};
        const geoNearStage = {
            $geoNear: {near: point, spherical: (geoType === "2dsphere")},
        };
        const res = coll.aggregate([geoNearStage, projStage]).toArray();
        assert.eq(res,
                  expected,
                  () => `Unexpected results from ${tojson(geoNearStage)} using a ${geoType} index`);
    }

    assertGeoNearResults(origin.pt, [origin, near, far, furthest]);
    assertGeoNearResults(furthest.pt, [furthest, far, near, origin]);
});
