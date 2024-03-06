// Axis aligned circles - hard-to-find precision errors possible with exact distances here

let t = db.axisaligned;
t.drop();

function checkCircle(center, radius, bound) {
    // Make sure our numbers are precise enough for this test
    if ((center[0] - radius == center[0]) || (center[1] - radius == center[1])) {
        print(`omitting circle due to precision`);
        return false;
    }

    // Make sure all points will fall within bounds
    if ((center[0] - radius < bound.min) || (center[1] - radius < bound.min) ||
        (center[0] + radius > bound.max) || (center[1] + radius > bound.max)) {
        print(`omitting circle due to bounds`);
        return false;
    }

    return true;
}

let radiiUnscaled = [0.0001, 0.001, 0.01, 0.1];
let centersUnscaled = [[5, 52], [6, 53], [7, 54], [8, 55], [9, 56]];

// Test matrix of various bits and scales. There are 32 combinations.
let bits = [2, 3, 4, 5, 6, 7, 8, 9];
let scale = [1, 10, 1000, 10000];
for (let b = 0; b < bits.length; b++) {
    for (let s = 0; s < scale.length; s++) {
        let radii = [];
        let centers = [];

        // Compute a set of centers and radii for the circles.
        for (let i = 0; i < centersUnscaled.length; i++) {
            centers.push([centersUnscaled[i][0] * scale[s], centersUnscaled[i][1] * scale[s]]);
        }

        for (let i = 0; i < radiiUnscaled.length; i++) {
            radii.push(radiiUnscaled[i] * scale[s]);
        }

        let bound = {min: -180 * scale[s], max: 180 * scale[s]};

        t.drop();

        let circles = [];

        // Insert legacy points for each combination of centers and radii.
        for (let c = 0; c < centers.length; c++) {
            for (let r = 0; r < radii.length; r++) {
                if (!checkCircle(centers[c], radii[r], bound)) {
                    continue;
                }

                // Define a set of points around a circle with the given center and radius.
                //
                //     x -->
                //
                // y   8    5    9
                // |
                // V   1    2    3
                //
                //     6    4    7
                //
                // Points 1-5 will be inside the circle (with point 2 at the center).
                // Points 6-9 will be outside.
                const circleIdx = circles.length;
                t.save({
                    circleIdx,
                    ptNum: 1,
                    "loc": {"x": centers[c][0] - radii[r], "y": centers[c][1]}
                });
                t.save({circleIdx, ptNum: 2, "loc": {"x": centers[c][0], "y": centers[c][1]}});
                t.save({
                    circleIdx,
                    ptNum: 3,
                    "loc": {"x": centers[c][0] + radii[r], "y": centers[c][1]}
                });
                t.save({
                    circleIdx,
                    ptNum: 4,
                    "loc": {"x": centers[c][0], "y": centers[c][1] + radii[r]}
                });
                t.save({
                    circleIdx,
                    ptNum: 5,
                    "loc": {"x": centers[c][0], "y": centers[c][1] - radii[r]}
                });
                t.save({
                    circleIdx,
                    ptNum: 6,
                    "loc": {"x": centers[c][0] - radii[r], "y": centers[c][1] + radii[r]}
                });
                t.save({
                    circleIdx,
                    ptNum: 7,
                    "loc": {"x": centers[c][0] + radii[r], "y": centers[c][1] + radii[r]}
                });
                t.save({
                    circleIdx,
                    ptNum: 8,
                    "loc": {"x": centers[c][0] - radii[r], "y": centers[c][1] - radii[r]}
                });
                t.save({
                    circleIdx,
                    ptNum: 9,
                    "loc": {"x": centers[c][0] + radii[r], "y": centers[c][1] - radii[r]}
                });

                circles.push({center: centers[c], radius: radii[r]});
            }
        }

        // Create a 2d legacy geo index with an additional component "circleIdx" so we can run
        // geo queries against each individual circle created above.
        assert.commandWorked(t.createIndex({loc: "2d", circleIdx: 1},
                                           {min: bound.min, max: bound.max, bits: bits[b]}));

        print(`Testing 2d geo index with ${bits[b]} bits at scale ${scale[s]} with ${
            circles.length} circles.`);
        for (let ci = 0; ci < circles.length; ci++) {
            const circle = circles[ci];

            // $within query
            let r = t.find({
                "loc": {"$within": {"$center": [circle.center, circle.radius]}},
                circleIdx: {$eq: ci}
            });
            assert.eq(5, r.count());

            // Make sure the 5 internal points are represented.
            let a = r.toArray();
            let x = [];
            for (k in a)
                x.push(a[k]["ptNum"]);
            x.sort();
            assert.eq([1, 2, 3, 4, 5], x);

            // $near query
            r = t.find({loc: {$near: circle.center, $maxDistance: circle.radius}, circleIdx: ci},
                       {_id: 1});
            assert.eq(5, r.count());

            // $geoNear query
            a = t.aggregate({
                     $geoNear: {
                         near: circle.center,
                         distanceField: "dis",
                         maxDistance: circle.radius,
                     }
                 },
                            {$match: {circleIdx: ci}})
                    .toArray();
            assert.eq(5, a.length, tojson(a));
            for (var k = 0; k < a.length; k++) {
                if (a[k].ptNum == 2) {
                    assert.eq(a[k].dis, 0, a[k]);
                } else {
                    // precision issues make it challenging to assert a distance equal to
                    // circle.radius here.
                    assert.close(a[k].dis, circle.radius);
                }
            }

            // $within $box query
            r = t.find({
                loc: {
                    $within: {
                        $box: [
                            [circle.center[0] - circle.radius, circle.center[1] - circle.radius],
                            [circle.center[0] + circle.radius, circle.center[1] + circle.radius]
                        ]
                    }
                },
                circleIdx: {$eq: ci},
            },
                       {_id: 1});
            assert.eq(9, r.count());
        }
    }
}
