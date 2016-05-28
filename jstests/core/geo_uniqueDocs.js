// Test uniqueDocs option for $within and geoNear queries SERVER-3139
// SERVER-12120 uniqueDocs is deprecated. Server always returns unique documents.

collName = 'geo_uniqueDocs_test';
t = db.geo_uniqueDocs_test;
t.drop();

t.save({locs: [[0, 2], [3, 4]]});
t.save({locs: [[6, 8], [10, 10]]});

t.ensureIndex({locs: '2d'});

// geoNear tests
// uniqueDocs option is ignored.
assert.eq(2, db.runCommand({geoNear: collName, near: [0, 0]}).results.length);
assert.eq(2, db.runCommand({geoNear: collName, near: [0, 0], uniqueDocs: false}).results.length);
assert.eq(2, db.runCommand({geoNear: collName, near: [0, 0], uniqueDocs: true}).results.length);
results = db.runCommand({geoNear: collName, near: [0, 0], num: 2}).results;
assert.eq(2, results.length);
assert.close(2, results[0].dis);
assert.close(10, results[1].dis);
results = db.runCommand({geoNear: collName, near: [0, 0], num: 2, uniqueDocs: true}).results;
assert.eq(2, results.length);
assert.close(2, results[0].dis);
assert.close(10, results[1].dis);

// $within tests

assert.eq(2, t.find({locs: {$within: {$box: [[0, 0], [9, 9]]}}}).itcount());
assert.eq(2, t.find({locs: {$within: {$box: [[0, 0], [9, 9]], $uniqueDocs: true}}}).itcount());
assert.eq(2, t.find({locs: {$within: {$box: [[0, 0], [9, 9]], $uniqueDocs: false}}}).itcount());

assert.eq(2, t.find({locs: {$within: {$center: [[5, 5], 7], $uniqueDocs: true}}}).itcount());
assert.eq(2, t.find({locs: {$within: {$center: [[5, 5], 7], $uniqueDocs: false}}}).itcount());

assert.eq(2, t.find({locs: {$within: {$centerSphere: [[5, 5], 1], $uniqueDocs: true}}}).itcount());
assert.eq(2, t.find({locs: {$within: {$centerSphere: [[5, 5], 1], $uniqueDocs: false}}}).itcount());

assert.eq(
    2,
    t.find({locs: {$within: {$polygon: [[0, 0], [0, 9], [9, 9]], $uniqueDocs: true}}}).itcount());
assert.eq(
    2,
    t.find({locs: {$within: {$polygon: [[0, 0], [0, 9], [9, 9]], $uniqueDocs: false}}}).itcount());
