load("jstests/sharding/move_chunk_with_session_helper.js");

(function() {
"use strict";

load("jstests/libs/retryable_writes_util.js");

if (!RetryableWritesUtil.storageEngineSupportsRetryableWrites(jsTest.options().storageEngine)) {
    jsTestLog("Retryable writes are not supported, skipping test");
    return;
}

// Prevent unnecessary elections in the first shard replica set. Shard 'rs1' shard will need its
// secondary to get elected, so we don't give it a zero priority.
var st = new ShardingTest({
    mongos: 2,
    shards: {
        rs0: {nodes: [{rsConfig: {}}, {rsConfig: {priority: 0}}]},
        rs1: {nodes: [{rsConfig: {}}, {rsConfig: {}}]}
    }
});
assert.commandWorked(st.s.adminCommand({enableSharding: 'test'}));
st.ensurePrimaryShard('test', st.shard0.shardName);

var coll = 'update';
var cmd = {
    update: 'update',
    updates: [
        {q: {x: 10}, u: {$inc: {a: 1}}},  // in place
        {q: {x: 20}, u: {$inc: {b: 1}}, upsert: true},
        {q: {x: 30}, u: {x: 30, z: 1}}  // replacement
    ],
    ordered: false,
    lsid: {id: UUID()},
    txnNumber: NumberLong(35),
};
var setup = function(coll) {
    coll.insert({x: 10});
    coll.insert({x: 30});
};
var checkRetryResult = function(result, retryResult) {
    assert.eq(result.ok, retryResult.ok);
    assert.eq(result.n, retryResult.n);
    assert.eq(result.nModified, retryResult.nModified);
    assert.eq(result.upserted, retryResult.upserted);
    assert.eq(result.writeErrors, retryResult.writeErrors);
    assert.eq(result.writeConcernErrors, retryResult.writeConcernErrors);
};
var checkDocuments = function(coll) {
    assert.eq(1, coll.findOne({x: 10}).a);
    assert.eq(1, coll.findOne({x: 20}).b);
    assert.eq(1, coll.findOne({x: 30}).z);
};

testMoveChunkWithSession(st, coll, cmd, setup, checkRetryResult, checkDocuments);

st.stop();
})();
