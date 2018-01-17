/**
 * This tests all points using $near.
 */

load("jstests/libs/geo_near_random.js");

(function() {
    'use strict';

    var testName = "geo_near_random1";
    var s = new ShardingTest({shards: 3});

    var db = s.getDB("test");

    var test = new GeoNearRandomTest(testName, db);

    assert.commandWorked(s.s0.adminCommand({enablesharding: 'test'}));
    s.ensurePrimaryShard('test', 'shard0001');
    assert.commandWorked(s.s0.adminCommand({shardcollection: ('test.' + testName), key: {_id: 1}}));

    test.insertPts(50);

    for (var i = (test.nPts / 10); i < test.nPts; i += (test.nPts / 10)) {
        assert.commandWorked(s.s0.adminCommand({split: ('test.' + testName), middle: {_id: i}}));
        try {
            assert.commandWorked(s.s0.adminCommand({
                moveChunk: ('test.' + testName),
                find: {_id: i - 1},
                to: ('shard000' + (i % 3)),
                _waitForDelete: true
            }));
        } catch (e) {
            // ignore this error
            if (!e.message.match(/that chunk is already on that shard/)) {
                throw e;
            }
        }
    }

    // Turn balancer back on, for actual tests
    // s.startBalancer(); // SERVER-13365

    var opts = {};
    test.testPt([0, 0], opts);
    test.testPt(test.mkPt(), opts);
    test.testPt(test.mkPt(), opts);
    test.testPt(test.mkPt(), opts);
    test.testPt(test.mkPt(), opts);

    s.stop();
})();
