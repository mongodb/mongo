load("jstests/sharding/move_chunk_with_session_helper.js");

(function() {
    "use strict";

    load("jstests/libs/retryable_writes_util.js");

    if (!RetryableWritesUtil.storageEngineSupportsRetryableWrites(jsTest.options().storageEngine)) {
        jsTestLog("Retryable writes are not supported, skipping test");
        return;
    }

    var st = new ShardingTest({shards: {rs0: {nodes: 2}, rs1: {nodes: 2}}});
    assert.commandWorked(st.s.adminCommand({enableSharding: 'test'}));
    st.ensurePrimaryShard('test', st.shard0.shardName);

    var coll = 'delete';
    var cmd = {
        delete: coll,
        deletes: [{q: {x: 10}, limit: 1}, {q: {x: 20}, limit: 1}],
        ordered: false,
        lsid: {id: UUID()},
        txnNumber: NumberLong(36),
    };
    var setup = function(coll) {
        var bulk = coll.initializeUnorderedBulkOp();
        for (let i = 0; i < 10; i++) {
            bulk.insert({x: 10});
            bulk.insert({x: 20});
        }
        assert.writeOK(bulk.execute());
    };
    var checkRetryResult = function(result, retryResult) {
        assert.eq(result.ok, retryResult.ok);
        assert.eq(result.n, retryResult.n);
        assert.eq(result.writeErrors, retryResult.writeErrors);
        assert.eq(result.writeConcernErrors, retryResult.writeConcernErrors);
    };
    var checkDocuments = function(coll) {
        assert.eq(9, coll.find({x: 10}).itcount());
        assert.eq(9, coll.find({x: 20}).itcount());
    };

    testMoveChunkWithSession(st, coll, cmd, setup, checkRetryResult, checkDocuments);

    st.stop();
})();
