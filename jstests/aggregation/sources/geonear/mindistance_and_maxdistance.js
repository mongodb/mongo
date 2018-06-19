/**
 * Tests the behavior of the $geoNear stage with varying values of 'minDistance' and 'maxDistance'.
 */
(function() {
    "use strict";

    const coll = db.getCollection("geonear_mindistance_maxdistance");

    const kMaxDistance = Math.PI * 2.0;

    // Test points that are exactly at the "near" point, close to the point, and far from the point.
    // Distances are purposely chosen to be small so that distances in meters and radians are close.
    const origin = {pt: [0, 0]};
    const near = {pt: [0.23, -0.32]};
    const far = {pt: [5.9, 0.0]};

    ["2d", "2dsphere"].forEach(geoType => {
        jsTestLog(`Testing $geoNear with index {pt: "${geoType}"}`);
        coll.drop();

        // Create the desired index type and populate the collection.
        assert.commandWorked(coll.createIndex({pt: geoType}));
        [origin, near, far].forEach(doc => {
            doc.distFromOrigin = (geoType === "2dsphere") ? Geo.sphereDistance(doc.pt, origin.pt)
                                                          : Geo.distance(doc.pt, origin.pt);
            assert.commandWorked(coll.insert(doc));
        });

        /**
         * Helper function that runs a $geoNear aggregation near the origin, setting the minimum
         * and/or maximum search distance using the object 'minMaxOpts', and asserting that the
         * results match 'expected'.
         */
        function assertGeoNearResults(minMaxOpts, expected) {
            const geoNearStage = {
                $geoNear: Object.extend(
                    {near: origin.pt, distanceField: "dist", spherical: (geoType === "2dsphere")},
                    minMaxOpts)
            };
            const projStage = {$project: {_id: 0, dist: 0}};
            const res = coll.aggregate([geoNearStage, projStage]).toArray();
            assert.eq(
                res,
                expected,
                () => `Unexpected results from ${tojson(geoNearStage)} using a ${geoType} index`);
        }

        // If no minimum nor maximum distance is set, all points are returned.
        assertGeoNearResults({}, [origin, near, far]);

        //
        // Tests for minDistance.
        //

        // Negative values and non-numeric values are illegal.
        assert.throws(() => assertGeoNearResults({minDistance: -1.1}));
        assert.throws(() => assertGeoNearResults({minDistance: "3.2"}));

        // A minimum distance of 0 returns all points.
        assertGeoNearResults({minDistance: -0.0}, [origin, near, far]);
        assertGeoNearResults({minDistance: 0.0}, [origin, near, far]);

        // Larger minimum distances exclude closer points.
        assertGeoNearResults({minDistance: (near.distFromOrigin / 2)}, [near, far]);
        assertGeoNearResults({minDistance: (far.distFromOrigin / 2)}, [far]);
        assertGeoNearResults({minDistance: kMaxDistance}, []);

        //
        // Tests for maxDistance.
        //

        // Negative values and non-numeric values are illegal.
        assert.throws(() => assertGeoNearResults({maxDistance: -1.1}));
        assert.throws(() => assertGeoNearResults({maxDistance: "3.2"}));

        // A maximum distance of 0 returns only the origin.
        assertGeoNearResults({maxDistance: 0.0}, [origin]);
        assertGeoNearResults({maxDistance: -0.0}, [origin]);

        // Larger maximum distances include more points.
        assertGeoNearResults({maxDistance: (near.distFromOrigin + 0.01)}, [origin, near]);
        assertGeoNearResults({maxDistance: (far.distFromOrigin + 0.01)}, [origin, near, far]);

        //
        // Tests for minDistance and maxDistance together.
        //

        // Cast a wide net and all points should be returned.
        assertGeoNearResults({minDistance: 0.0, maxDistance: kMaxDistance}, [origin, near, far]);

        // A narrower range excludes the origin and the far point.
        assertGeoNearResults(
            {minDistance: (near.distFromOrigin / 2), maxDistance: (near.distFromOrigin + 0.01)},
            [near]);

        // An impossible range is legal but returns no results.
        assertGeoNearResults({minDistance: 3.0, maxDistance: 1.0}, []);
    });
}());
