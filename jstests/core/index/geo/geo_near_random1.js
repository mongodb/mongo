/**
 * this tests all points
 *
 * @tags: [
 *  # GeoNearRandomTest::testPt() uses find().toArray() that makes use of a cursor
 *  requires_getmore,
 * ]
 */
import {GeoNearRandomTest} from "jstests/libs/query/geo_near_random.js";

let test = new GeoNearRandomTest("geo_near_random1");

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

let opts = {sphere: 1};

// Test $nearSphere with a 2d index
test.testPt([0, 0], opts);
test.testPt(test.mkPt(), opts);
test.testPt(test.mkPt(), opts);
test.testPt(test.mkPt(), opts);
test.testPt(test.mkPt(), opts);

// Test $nearSphere with a 2dsphere index
assert.commandWorked(db.geo_near_random1.dropIndex({loc: "2d"}));
assert.commandWorked(db.geo_near_random1.createIndex({loc: "2dsphere"}));
test.testPt([0, 0], opts);
test.testPt(test.mkPt(), opts);
test.testPt(test.mkPt(), opts);
test.testPt(test.mkPt(), opts);
test.testPt(test.mkPt(), opts);
