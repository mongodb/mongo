// Test SERVER-14306.  Do a query directly against a mongod with an in-memory sort and a limit that
// doesn't cause the in-memory sort limit to be reached, then make sure the same limit also doesn't
// cause the in-memory sort limit to be reached when running through a mongos.
(function() {
    'use strict';

    var st = new ShardingTest({shards: 2});
    assert.commandWorked(st.s.adminCommand({enableSharding: 'test'}));
    st.ensurePrimaryShard('test', 'shard0000');

    // Make sure that at least 1 chunk is on another shard so that mongos doesn't treat this as a
    // single-shard query (which doesn't exercise the bug)
    assert.commandWorked(st.s.adminCommand(
        {shardCollection: 'test.skip', key: {_id: 'hashed'}, numInitialChunks: 64}));

    var mongosCol = st.s.getDB('test').getCollection('skip');
    var shardCol = st.shard0.getDB('test').getCollection('skip');

    // Create enough data to exceed the 32MB in-memory sort limit (per shard)
    var filler = new Array(10240).toString();
    var bulkOp = mongosCol.initializeOrderedBulkOp();
    for (var i = 0; i < 12800; i++) {
        bulkOp.insert({x: i, str: filler});
    }
    assert.writeOK(bulkOp.execute());

    var passLimit = 2000;
    var failLimit = 4000;

    // Test on MongoD
    jsTestLog("Test no error with limit of " + passLimit + " on mongod");
    assert.eq(passLimit, shardCol.find().sort({x: 1}).limit(passLimit).itcount());

    jsTestLog("Test error with limit of " + failLimit + " on mongod");
    assert.throws(function() {
        shardCol.find().sort({x: 1}).limit(failLimit).itcount();
    });

    // Test on MongoS
    jsTestLog("Test no error with limit of " + passLimit + " on mongos");
    assert.eq(passLimit, mongosCol.find().sort({x: 1}).limit(passLimit).itcount());

    jsTestLog("Test error with limit of " + failLimit + " on mongos");
    assert.throws(function() {
        mongosCol.find().sort({x: 1}).limit(failLimit).itcount();
    });

    st.stop();

})();
