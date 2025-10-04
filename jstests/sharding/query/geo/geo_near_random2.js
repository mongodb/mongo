/**
 * This tests 1% of all points using $near and $nearSphere.
 */

import {GeoNearRandomTest} from "jstests/libs/query/geo_near_random.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

let testName = jsTestName();
let s = new ShardingTest({shards: 3});

var db = s.getDB("test");

let test = new GeoNearRandomTest(testName, db);

assert.commandWorked(s.s0.adminCommand({enablesharding: "test", primaryShard: s.shard1.shardName}));
assert.commandWorked(s.s0.adminCommand({shardcollection: "test." + testName, key: {_id: 1}}));

test.insertPts(5000);
let shardList = [s.shard0.shardName, s.shard1.shardName, s.shard2.shardName];
for (let i = test.nPts / 10; i < test.nPts; i += test.nPts / 10) {
    assert.commandWorked(s.s0.adminCommand({split: "test." + testName, middle: {_id: i}}));
    try {
        assert.commandWorked(
            s.s0.adminCommand({
                moveChunk: "test." + testName,
                find: {_id: i - 1},
                to: shardList[i % 3],
                _waitForDelete: true,
            }),
        );
    } catch (e) {
        // ignore this error
        if (!e.message.match(/that chunk is already on that shard/)) {
            throw e;
        }
    }
}

// Turn balancer back on, for actual tests
// s.startBalancer(); // SERVER-13365

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

s.stop();
