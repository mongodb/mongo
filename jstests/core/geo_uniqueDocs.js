// Test uniqueDocs option for $within queries and the $geoNear aggregation stage. SERVER-3139
// SERVER-12120 uniqueDocs is deprecated. Server always returns unique documents.

collName = 'geo_uniqueDocs_test';
t = db.geo_uniqueDocs_test;
t.drop();

assert.commandWorked(t.save({locs: [[0, 2], [3, 4]]}));
assert.commandWorked(t.save({locs: [[6, 8], [10, 10]]}));

assert.commandWorked(t.createIndex({locs: '2d'}));

// $geoNear tests
// uniqueDocs option is ignored.
assert.eq(2, t.aggregate({$geoNear: {near: [0, 0], distanceField: "dis"}}).toArray().length);
assert.eq(2,
          t.aggregate({$geoNear: {near: [0, 0], distanceField: "dis", uniqueDocs: false}})
              .toArray()
              .length);
assert.eq(2,
          t.aggregate({$geoNear: {near: [0, 0], distanceField: "dis", uniqueDocs: true}})
              .toArray()
              .length);
results = t.aggregate([{$geoNear: {near: [0, 0], distanceField: "dis"}}, {$limit: 2}]).toArray();
assert.eq(2, results.length);
assert.close(2, results[0].dis);
assert.close(10, results[1].dis);
results = t.aggregate([
               {$geoNear: {near: [0, 0], distanceField: "dis", uniqueDocs: true}},
               {$limit: 2}
           ]).toArray();
assert.eq(2, results.length);
assert.close(2, results[0].dis);
assert.close(10, results[1].dis);

// $within tests

assert.eq(2, t.find({locs: {$within: {$box: [[0, 0], [9, 9]]}}}).itcount());
assert.eq(2, t.find({locs: {$within: {$box: [[0, 0], [9, 9]], $uniqueDocs: true}}}).itcount());
assert.eq(2, t.find({locs: {$within: {$box: [[0, 0], [9, 9]], $uniqueDocs: false}}}).itcount());

assert.eq(2, t.find({locs: {$within: {$center: [[5, 5], 7], $uniqueDocs: true}}}).itcount());
assert.eq(2, t.find({locs: {$within: {$center: [[5, 5], 7], $uniqueDocs: false}}}).itcount());
assert.eq(2, t.find({locs: {$within: {$uniqueDocs: false, $center: [[5, 5], 7]}}}).itcount());

assert.eq(2, t.find({locs: {$within: {$centerSphere: [[5, 5], 1], $uniqueDocs: true}}}).itcount());
assert.eq(2, t.find({locs: {$within: {$centerSphere: [[5, 5], 1], $uniqueDocs: false}}}).itcount());
assert.eq(2, t.find({locs: {$within: {$uniqueDocs: true, $centerSphere: [[5, 5], 1]}}}).itcount());
assert.eq(
    2,
    t.find({locs: {$within: {$polygon: [[0, 0], [0, 9], [9, 9]], $uniqueDocs: true}}}).itcount());
assert.eq(
    2,
    t.find({locs: {$within: {$polygon: [[0, 0], [0, 9], [9, 9]], $uniqueDocs: false}}}).itcount());
