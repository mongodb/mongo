/**
 * Test write concern with w parameter will not cause an error when writes to mongos would trigger
 * writes to config servers (in this test, autosplit is used).
 */
(function() {
    'use strict';

    var st = new ShardingTest({shards: 1, rs: true, other: {chunkSize: 1, enableAutoSplit: true}});

    var mongos = st.s;
    var testDB = mongos.getDB('test');
    var coll = testDB.user;

    assert.commandWorked(st.s0.adminCommand({enableSharding: testDB.getName()}));
    assert.commandWorked(st.s0.adminCommand({shardCollection: coll.getFullName(), key: {x: 1}}));

    var chunkCount = function() {
        return mongos.getDB('config').chunks.find().count();
    };

    var initChunks = chunkCount();
    var currChunks = initChunks;
    var gleObj = null;
    var x = 0;
    var largeStr = new Array(1024 * 128).toString();

    assert.soon(
        function() {
            var bulk = coll.initializeUnorderedBulkOp();
            for (var i = 0; i < 100; i++) {
                bulk.insert({x: x++, largeStr: largeStr});
            }
            assert.writeOK(bulk.execute({w: 'majority', wtimeout: 60 * 1000}));
            currChunks = chunkCount();
            return currChunks > initChunks;
        },
        function() {
            return "currChunks: " + currChunks + ", initChunks: " + initChunks;
        });

    st.stop();
})();
