// this tests 1% of all points using $near and $nearSphere
var db;
(function() {
    "use strict";
    load("jstests/libs/geo_near_random.js");

    const conn = MongoRunner.runMongod({smallfiles: "", nojournal: ""});
    assert.neq(null, conn, "mongod failed to start.");
    db = conn.getDB("test");

    var test = new GeoNearRandomTest("weekly.geo_near_random2");

    test.insertPts(50000);

    const opts = {sphere: 0, nToTest: test.nPts * 0.01};
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
})();
