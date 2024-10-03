// this tests 1% of all points using $near and $nearSphere
import {GeoNearRandomTest} from "jstests/libs/query/geo_near_random.js";

const conn = MongoRunner.runMongod();
assert.neq(null, conn, "mongod failed to start.");

var test = new GeoNearRandomTest("weekly.query/geo_near_random2", conn.getDB("test"));

test.insertPts(50000);

const opts = {
    sphere: 0,
    nToTest: test.nPts * 0.01
};
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

MongoRunner.stopMongod(conn);