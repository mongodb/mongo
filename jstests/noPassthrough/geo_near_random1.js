// this tests all points using $near
(function() {
"use strict";
load("jstests/libs/geo_near_random.js");

const conn = MongoRunner.runMongod();
assert.neq(null, conn, "mongod failed to start.");

var test = new GeoNearRandomTest("weekly.geo_near_random1", conn.getDB("test"));

test.insertPts(1000);

test.testPt([0, 0]);
test.testPt(test.mkPt());
test.testPt(test.mkPt());
test.testPt(test.mkPt());
test.testPt(test.mkPt());

MongoRunner.stopMongod(conn);
})();
