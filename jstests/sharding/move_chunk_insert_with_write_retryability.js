import {ShardingTest} from "jstests/libs/shardingtest.js";
import {testMoveChunkWithSession} from "jstests/sharding/move_chunk_with_session_helper.js";

// Prevent unnecessary elections in the first shard replica set. Shard 'rs1' shard will need its
// secondary to get elected, so we don't give it a zero priority.
let st = new ShardingTest({
    mongos: 2,
    shards: {
        rs0: {nodes: [{rsConfig: {}}, {rsConfig: {priority: 0}}]},
        rs1: {nodes: [{rsConfig: {}}, {rsConfig: {}}]},
    },
});
assert.commandWorked(st.s.adminCommand({enableSharding: "test", primaryShard: st.shard0.shardName}));

let coll = "insert";
let cmd = {
    insert: coll,
    documents: [{x: 10}, {x: 30}],
    ordered: false,
    lsid: {id: UUID()},
    txnNumber: NumberLong(34),
};
let setup = function () {};
let checkRetryResult = function (result, retryResult) {
    assert.eq(result.ok, retryResult.ok);
    assert.eq(result.n, retryResult.n);
    assert.eq(result.writeErrors, retryResult.writeErrors);
    assert.eq(result.writeConcernErrors, retryResult.writeConcernErrors);
};
let checkDocuments = function (coll) {
    assert.eq(1, coll.find({x: 10}).itcount());
    assert.eq(1, coll.find({x: 30}).itcount());
};

testMoveChunkWithSession(st, coll, cmd, setup, checkRetryResult, checkDocuments);

st.stop();
