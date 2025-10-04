/**
 * @tags: [
 *     # Sharded clusters do not clean up correctly after failed index builds.
 *     # See SERVER-33207 as an example.
 *     assumes_unsharded_collection,
 *     does_not_support_stepdowns,
 * ]
 */

let t = db.borders;
t.drop();

let epsilon = 0.0001;

// For these tests, *required* that step ends exactly on max
let min = -1;
let max = 1;
let step = 1;
let numItems = 0;

for (let x = min; x <= max; x += step) {
    for (let y = min; y <= max; y += step) {
        t.insert({loc: {x: x, y: y}});
        numItems++;
    }
}

let overallMin = -1;
let overallMax = 1;

// Create a point index slightly smaller than the points we have
let res = t.createIndex({loc: "2d"}, {max: overallMax - epsilon / 2, min: overallMin + epsilon / 2});
assert.commandFailed(res);

// Create a point index only slightly bigger than the points we have
res = t.createIndex({loc: "2d"}, {max: overallMax + epsilon, min: overallMin - epsilon});
assert.commandWorked(res);

// ************
// Box Tests
// ************

// If the bounds are bigger than the box itself, just clip at the borders
assert.eq(
    numItems,
    t
        .find({
            loc: {
                $within: {
                    $box: [
                        [overallMin - 2 * epsilon, overallMin - 2 * epsilon],
                        [overallMax + 2 * epsilon, overallMax + 2 * epsilon],
                    ],
                },
            },
        })
        .count(),
);

// Check this works also for bounds where only a single dimension is off-bounds
assert.eq(
    numItems - 5,
    t
        .find({
            loc: {
                $within: {
                    $box: [
                        [overallMin - 2 * epsilon, overallMin - 0.5 * epsilon],
                        [overallMax - epsilon, overallMax - epsilon],
                    ],
                },
            },
        })
        .count(),
);

// Make sure we can get at least close to the bounds of the index
assert.eq(
    numItems,
    t
        .find({
            loc: {
                $within: {
                    $box: [
                        [overallMin - epsilon / 2, overallMin - epsilon / 2],
                        [overallMax + epsilon / 2, overallMax + epsilon / 2],
                    ],
                },
            },
        })
        .count(),
);

// Make sure we can get at least close to the bounds of the index
assert.eq(
    numItems,
    t
        .find({
            loc: {
                $within: {
                    $box: [
                        [overallMax + epsilon / 2, overallMax + epsilon / 2],
                        [overallMin - epsilon / 2, overallMin - epsilon / 2],
                    ],
                },
            },
        })
        .count(),
);

// Check that swapping min/max has good behavior
assert.eq(
    numItems,
    t
        .find({
            loc: {
                $within: {
                    $box: [
                        [overallMax + epsilon / 2, overallMax + epsilon / 2],
                        [overallMin - epsilon / 2, overallMin - epsilon / 2],
                    ],
                },
            },
        })
        .count(),
);

assert.eq(
    numItems,
    t
        .find({
            loc: {
                $within: {
                    $box: [
                        [overallMax + epsilon / 2, overallMin - epsilon / 2],
                        [overallMin - epsilon / 2, overallMax + epsilon / 2],
                    ],
                },
            },
        })
        .count(),
);

// **************
// Circle tests
// **************

let center = (overallMax + overallMin) / 2;
center = [center, center];
let radius = overallMax;

let offCenter = [center[0] + radius, center[1] + radius];
let onBounds = [offCenter[0] + epsilon, offCenter[1] + epsilon];
let offBounds = [onBounds[0] + epsilon, onBounds[1] + epsilon];
let onBoundsNeg = [-onBounds[0], -onBounds[1]];

// Make sure we can get all points when radius is exactly at full bounds
assert.lt(0, t.find({loc: {$within: {$center: [center, radius + epsilon]}}}).count());

// Make sure we can get points when radius is over full bounds
assert.lt(0, t.find({loc: {$within: {$center: [center, radius + 2 * epsilon]}}}).count());

// Make sure we can get points when radius is over full bounds, off-centered
assert.lt(0, t.find({loc: {$within: {$center: [offCenter, radius + 2 * epsilon]}}}).count());

// Make sure we get correct corner point when center is in bounds
// (x bounds wrap, so could get other corner)
let cornerPt = t.findOne({loc: {$within: {$center: [offCenter, step / 2]}}});
assert.eq(cornerPt.loc.y, overallMax);

// Make sure we get correct corner point when center is on bounds
// NOTE: Only valid points on MIN bounds
cornerPt = t.findOne({loc: {$within: {$center: [onBoundsNeg, Math.sqrt(2 * epsilon * epsilon) + step / 2]}}});
assert.eq(cornerPt.loc.y, overallMin);

// ***********
// Near tests
// ***********

// Make sure we can get all nearby points to point in range
assert.eq(overallMax, t.find({loc: {$near: offCenter}}).next().loc.y);

// Make sure we can get all nearby points to point on boundary
assert.eq(overallMin, t.find({loc: {$near: onBoundsNeg}}).next().loc.y);

// Make sure we can't get all nearby points to point on max boundary
// Broken - see SERVER-13581
// assert.throws(function(){
//    t.findOne( { loc : { $near : onBoundsNeg } } );
//});

// Make sure we can get all nearby points within one step (4 points in top
// corner)
assert.eq(4, t.find({loc: {$near: offCenter, $maxDistance: step * 1.9}}).count());

// **************
// Command Tests
// **************
// Make sure we can get all nearby points to point in range
assert.eq(overallMax, t.aggregate({$geoNear: {near: offCenter, distanceField: "d"}}).toArray()[0].loc.y);

// Make sure we can get all nearby points to point on boundary
assert.eq(overallMin, t.aggregate({$geoNear: {near: onBoundsNeg, distanceField: "d"}}).toArray()[0].loc.y);

// Make sure we can't get all nearby points to point over boundary
assert.commandFailedWithCode(
    db.runCommand({
        aggregate: "borders",
        cursor: {},
        pipeline: [{$geoNear: {near: offBounds, distanceField: "d"}}],
    }),
    16433,
);
assert.eq(numItems, t.aggregate({$geoNear: {near: onBounds, distanceField: "d"}}).toArray().length);

// Make sure we can get all nearby points within one step (4 points in top
// corner)
assert.eq(4, t.aggregate({$geoNear: {near: offCenter, maxDistance: step * 1.5, distanceField: "d"}}).toArray().length);
