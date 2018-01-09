// this tests all points
load("jstests/libs/geo_near_random.js");

var test = new GeoNearRandomTest("geo_near_random1");

test.insertPts(50);

// test.testPt() runs geoNear commands at the given coordinates with
// limits from 1 to nPts(# of inserted points). At the nth run, it
// compares the first (n - 1) results with the result of the (n - 1)th
// run to make sure they are identical. It also makes sure that the
// distances are in increasing order. The test runs in O(N^2).

// Test $near with a 2dindex
test.testPt([0, 0]);
test.testPt(test.mkPt());
test.testPt(test.mkPt());
test.testPt(test.mkPt());
test.testPt(test.mkPt());

opts = {
    sphere: 1
};

// Test $nearSphere with a 2d index
test.testPt([0, 0], opts);
test.testPt(test.mkPt(), opts);
test.testPt(test.mkPt(), opts);
test.testPt(test.mkPt(), opts);
test.testPt(test.mkPt(), opts);

// Test $nearSphere with a 2dsphere index
assert.commandWorked(db.geo_near_random1.dropIndex({loc: '2d'}));
assert.commandWorked(db.geo_near_random1.ensureIndex({loc: '2dsphere'}));
test.testPt([0, 0], opts);
test.testPt(test.mkPt(), opts);
test.testPt(test.mkPt(), opts);
test.testPt(test.mkPt(), opts);
test.testPt(test.mkPt(), opts);