// this tests 1% of all points using $near and $nearSphere
import {GeoNearRandomTest} from "jstests/libs/query/geo_near_random.js";

let test = new GeoNearRandomTest("nightly.geo_near_random2");

test.insertPts(10000);

let opts = {sphere: 0, nToTest: test.nPts * 0.01};
test.testPt([0, 0], opts);
test.testPt(test.mkPt(), opts);
test.testPt(test.mkPt(), opts);
test.testPt(test.mkPt(), opts);
test.testPt(test.mkPt(), opts);

opts.sphere = 1;
test.testPt([0, 0], opts);
test.testPt(test.mkPt(0.8), opts);
test.testPt(test.mkPt(0.8), opts);
test.testPt(test.mkPt(0.8), opts);
test.testPt(test.mkPt(0.8), opts);
