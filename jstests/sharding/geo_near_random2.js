// this tests 1% of all points using $near and $nearSphere
load("jstests/libs/geo_near_random.js");

var testName = "geo_near_random2";
var s = new ShardingTest( testName , 3 );

s.stopBalancer();

db = s.getDB("test"); // global db

var test = new GeoNearRandomTest(testName);

s.adminCommand({enablesharding:'test'});
s.adminCommand({shardcollection: ('test.' + testName), key: {_id:1} });

test.insertPts(5000);

for (var i = (test.nPts/10); i < test.nPts; i+= (test.nPts/10)){
    s.adminCommand({split: ('test.' + testName), middle: {_id: i} });
    try {
        s.adminCommand({moveChunk: ('test.' + testName), find: {_id: i-1}, to: ('shard000' + (i%3)), _waitForDelete : true });
    } catch (e) {
        // ignore this error
        if (! e.match(/that chunk is already on that shard/)){
            throw e;
        }
    }
}

//Turn balancer back on, for actual tests
s.setBalancer( true );

printShardingSizes()

opts = {sphere:0, nToTest:test.nPts*0.01, sharded:true}; 
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

s.stop()
