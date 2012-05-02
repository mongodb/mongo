// this tests 1% of all points using $near and $nearSphere
load("jstests/libs/geo_near_random.js");
load( "jstests/libs/slow_weekly_util.js" )

testServer = new SlowWeeklyMongod( "geo_near_random2" )
db = testServer.getDB( "test" );

var test = new GeoNearRandomTest("weekly.geo_near_random2");

test.insertPts(50000);

opts = {sphere:0, nToTest:test.nPts*0.01}; 
test.testPt([0,0], opts);
test.testPt(test.mkPt(), opts);
test.testPt(test.mkPt(), opts);
test.testPt(test.mkPt(), opts);
test.testPt(test.mkPt(), opts);

opts.sphere = 1
test.testPt([0,0], opts);
test.testPt(test.mkPt(0.8), opts);
test.testPt(test.mkPt(0.8), opts);
test.testPt(test.mkPt(0.8), opts);
test.testPt(test.mkPt(0.8), opts);

testServer.stop();
