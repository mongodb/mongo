// this tests all points using $near
load("jstests/libs/geo_near_random.js");

var test = new GeoNearRandomTest("geo_near_random1");

test.insertPts(50);

test.testPt([0,0]);
test.testPt(test.mkPt());
test.testPt(test.mkPt());
test.testPt(test.mkPt());
test.testPt(test.mkPt());
