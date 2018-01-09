// this tests 1% of all points
load("jstests/libs/geo_near_random.js");

var test = new GeoNearRandomTest("geo_near_random2");

test.insertPts(5000);

// test.testPt() runs geoNear commands at the given coordinates with
// limits from 1 to nPts(# of inserted points). At the nth run, it
// compares the first (n - 1) results with the result of the (n - 1)th
// run to make sure they are identical. It also makes sure that the
// distances are in increasing order. The test runs in O(N^2).

// Test $near with 2d index
opts = {
    sphere: 0,
    nToTest: test.nPts * 0.01
};
test.testPt([0, 0], opts);
test.testPt(test.mkPt(), opts);
test.testPt(test.mkPt(), opts);
test.testPt(test.mkPt(), opts);
test.testPt(test.mkPt(), opts);

opts.sphere = 1;

// Test $nearSphere with 2d index
test.testPt([0, 0], opts);
// test.mkPt(0.8) generates a random point in the maximum
// lat long bounds scaled by 0.8
test.testPt(test.mkPt(0.8), opts);
test.testPt(test.mkPt(0.8), opts);
test.testPt(test.mkPt(0.8), opts);
test.testPt(test.mkPt(0.8), opts);

// Test $nearSphere with 2dsphere index
assert.commandWorked(db.geo_near_random2.dropIndex({loc: '2d'}));
assert.commandWorked(db.geo_near_random2.ensureIndex({loc: '2dsphere'}));
test.testPt([0, 0], opts);
test.testPt(test.mkPt(0.8), opts);
test.testPt(test.mkPt(0.8), opts);
test.testPt(test.mkPt(0.8), opts);
test.testPt(test.mkPt(0.8), opts);