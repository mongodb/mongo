// this tests all points using $near
load("jstests/libs/geo_near_random.js");

var test = new GeoNearRandomTest("nightly.geo_near_random1");

test.insertPts(200);

test.testPt([0, 0]);
test.testPt(test.mkPt());
test.testPt(test.mkPt());
test.testPt(test.mkPt());
test.testPt(test.mkPt());
