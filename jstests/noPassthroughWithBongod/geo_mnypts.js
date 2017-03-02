// Test sanity of geo queries with a lot of points

var coll = db.testMnyPts;
coll.drop();

var totalPts = 500 * 1000;

// Add points in a 100x100 grid
var bulk = coll.initializeUnorderedBulkOp();
for (var i = 0; i < totalPts; i++) {
    var ii = i % 10000;
    bulk.insert({loc: [ii % 100, Math.floor(ii / 100)]});
}
assert.writeOK(bulk.execute());

coll.ensureIndex({loc: "2d"});

// Check that quarter of points in each quadrant
for (var i = 0; i < 4; i++) {
    var x = i % 2;
    var y = Math.floor(i / 2);

    var box = [[0, 0], [49, 49]];
    box[0][0] += (x == 1 ? 50 : 0);
    box[1][0] += (x == 1 ? 50 : 0);
    box[0][1] += (y == 1 ? 50 : 0);
    box[1][1] += (y == 1 ? 50 : 0);

    assert.eq(totalPts / 4, coll.find({loc: {$within: {$box: box}}}).count());
    assert.eq(totalPts / 4, coll.find({loc: {$within: {$box: box}}}).itcount());
}

// Check that half of points in each half
for (var i = 0; i < 2; i++) {
    var box = [[0, 0], [49, 99]];
    box[0][0] += (i == 1 ? 50 : 0);
    box[1][0] += (i == 1 ? 50 : 0);

    assert.eq(totalPts / 2, coll.find({loc: {$within: {$box: box}}}).count());
    assert.eq(totalPts / 2, coll.find({loc: {$within: {$box: box}}}).itcount());
}

// Check that all but corner set of points in radius
var circle = [[0, 0], (100 - 1) * Math.sqrt(2) - 0.25];

assert.eq(totalPts - totalPts / (100 * 100),
          coll.find({loc: {$within: {$center: circle}}}).count());
assert.eq(totalPts - totalPts / (100 * 100),
          coll.find({loc: {$within: {$center: circle}}}).itcount());
