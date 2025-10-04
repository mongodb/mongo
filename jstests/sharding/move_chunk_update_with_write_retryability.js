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

let coll = "update";
let cmd = {
    update: "update",
    updates: [
        {q: {x: 10}, u: {$inc: {a: 1}}}, // in place
        {q: {x: 20}, u: {$inc: {b: 1}}, upsert: true},
        {q: {x: 30}, u: {x: 30, z: 1}}, // replacement
    ],
    ordered: false,
    lsid: {id: UUID()},
    txnNumber: NumberLong(35),
};
let setup = function (coll) {
    coll.insert({x: 10});
    coll.insert({x: 30});
};
let checkRetryResult = function (result, retryResult) {
    assert.eq(result.ok, retryResult.ok);
    assert.eq(result.n, retryResult.n);
    assert.eq(result.nModified, retryResult.nModified);
    assert.eq(result.upserted, retryResult.upserted);
    assert.eq(result.writeErrors, retryResult.writeErrors);
    assert.eq(result.writeConcernErrors, retryResult.writeConcernErrors);
};
let checkDocuments = function (coll) {
    assert.eq(1, coll.findOne({x: 10}).a);
    assert.eq(1, coll.findOne({x: 20}).b);
    assert.eq(1, coll.findOne({x: 30}).z);
};

testMoveChunkWithSession(st, coll, cmd, setup, checkRetryResult, checkDocuments);

st.stop();
