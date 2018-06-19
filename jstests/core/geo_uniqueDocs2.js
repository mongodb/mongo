// @tags: [requires_non_retryable_writes]

// Additional checks for geo uniqueDocs and includeLocs SERVER-3139.
// SERVER-12120 uniqueDocs is deprecated.
// Server always returns results with implied uniqueDocs=true

collName = 'jstests_geo_uniqueDocs2';
t = db[collName];
t.drop();

t.save({loc: [[20, 30], [40, 50]]});
t.ensureIndex({loc: '2d'});

// Check exact matches of different locations.
assert.eq(1, t.count({loc: [20, 30]}));
assert.eq(1, t.count({loc: [40, 50]}));

// Check behavior for $near, where $uniqueDocs mode is unavailable.
assert.eq([t.findOne()], t.find({loc: {$near: [50, 50]}}).toArray());

// Check correct number of matches for $within / $uniqueDocs.
// uniqueDocs ignored - does not affect results.
assert.eq(1, t.count({loc: {$within: {$center: [[30, 30], 40]}}}));
assert.eq(1, t.count({loc: {$within: {$center: [[30, 30], 40], $uniqueDocs: true}}}));
assert.eq(1, t.count({loc: {$within: {$center: [[30, 30], 40], $uniqueDocs: false}}}));

// For $within / $uniqueDocs, limit applies to docs.
assert.eq(
    1, t.find({loc: {$within: {$center: [[30, 30], 40], $uniqueDocs: false}}}).limit(1).itcount());

// Now check a circle only containing one of the locs.
assert.eq(1, t.count({loc: {$within: {$center: [[30, 30], 10]}}}));
assert.eq(1, t.count({loc: {$within: {$center: [[30, 30], 10], $uniqueDocs: true}}}));
assert.eq(1, t.count({loc: {$within: {$center: [[30, 30], 10], $uniqueDocs: false}}}));

// Check number and character of results with geoNear / uniqueDocs / includeLocs.
notUniqueNotInclude =
    t.aggregate({$geoNear: {near: [50, 50], distanceField: "dis", uniqueDocs: false}}).toArray();
uniqueNotInclude =
    t.aggregate({$geoNear: {near: [50, 50], distanceField: "dis", uniqueDocs: true}}).toArray();
notUniqueInclude = t.aggregate({
                        $geoNear: {
                            near: [50, 50],
                            distanceField: "dis",
                            uniqueDocs: false,
                            includeLocs: "point",
                        }
                    }).toArray();
uniqueInclude = t.aggregate({
                     $geoNear: {
                         near: [50, 50],
                         distanceField: "dis",
                         uniqueDocs: true,
                         includeLocs: "point",
                     }
                 }).toArray();

// Check that only unique results are returned, regardless of the value of "uniqueDocs" parameter.
assert.eq(1, notUniqueNotInclude.length);
assert.eq(1, uniqueNotInclude.length);
assert.eq(1, notUniqueInclude.length);
assert.eq(1, uniqueInclude.length);

// Check that locs are included.
assert(!notUniqueNotInclude[0].point);
assert(!uniqueNotInclude[0].point);
assert(notUniqueInclude[0].point);
assert(uniqueInclude[0].point);

// Check locs returned in includeLocs mode.
t.remove({});
objLocs = [{x: 20, y: 30, z: ['loc1', 'loca']}, {x: 40, y: 50, z: ['loc2', 'locb']}];
t.save({loc: objLocs});
results = t.aggregate({
               $geoNear: {
                   near: [50, 50],
                   distanceField: "dis",
                   uniqueDocs: false,
                   includeLocs: "point",
               }
           }).toArray();
assert.contains(results[0].point, objLocs);

// Check locs returned in includeLocs mode, where locs are arrays.
t.remove({});
arrLocs = [[20, 30], [40, 50]];
t.save({loc: arrLocs});
results = t.aggregate({
               $geoNear: {
                   near: [50, 50],
                   distanceField: "dis",
                   uniqueDocs: false,
                   includeLocs: "point",
               }
           }).toArray();
// The original loc arrays are returned as objects.
expectedLocs = arrLocs;

assert.contains(results[0].point, expectedLocs);

// Test a large number of locations in the array.
t.drop();
arr = [];
for (i = 0; i < 10000; ++i) {
    arr.push([10, 10]);
}
arr.push([100, 100]);
t.save({loc: arr});
t.ensureIndex({loc: '2d'});
assert.eq(1, t.count({loc: {$within: {$center: [[99, 99], 5]}}}));
