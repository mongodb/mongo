/**
 * Tests batched update and delete ops with id without shard key uses PM-3190 for retryable
 * writes.
 *
 * @tags: [requires_fcv_80]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";
import {CreateShardedCollectionUtil} from "jstests/sharding/libs/create_sharded_collection_util.js";

const st = new ShardingTest({shards: 2});
const mongos = st.s0;
let db = mongos.getDB(jsTestName());

const coll = db.coll;
coll.drop();

CreateShardedCollectionUtil.shardCollectionWithChunks(coll, {x: 1}, [
    {min: {x: MinKey}, max: {x: -100}, shard: st.shard0.shardName},
    {min: {x: -100}, max: {x: 0}, shard: st.shard0.shardName},
    {min: {x: 0}, max: {x: MaxKey}, shard: st.shard1.shardName},
]);
const performOps = function(ordered, numOps) {
    jsTest.log("Perform write with ordered: " + ordered);
    // Write two documents.
    assert.commandWorked(coll.insert({x: -1, _id: -1}));
    assert.commandWorked(coll.insert({x: 1, _id: 1}));

    assert.commandWorked(
        db.adminCommand({moveChunk: coll.getFullName(), find: {x: -1}, to: st.shard1.shardName}));

    // Test that retryable writes use broadcast protocol per PM-3190
    let session = st.s.startSession({retryWrites: true});
    const lsid = session.getSessionId();
    let ret = assert.commandWorked(
        db.getSiblingDB(db.getName()).getCollection(coll.getName()).runCommand("update", {
            updates: [
                {q: {_id: -1}, u: {$inc: {counter: 1}}},
                {q: {_id: 1}, u: {$inc: {counter: 1}}},
            ],
            lsid: lsid,
            txnNumber: NumberLong(5),
            ordered: ordered,
        }));
    assert.eq(ret.n, 2);

    ret = assert.commandWorked(
        db.getSiblingDB(db.getName()).getCollection(coll.getName()).runCommand("delete", {
            deletes: [
                {q: {_id: -1}, limit: 1},
                {q: {_id: 1}, limit: 1},
            ],
            lsid: lsid,
            txnNumber: NumberLong(6),
            ordered: ordered,
        }));
    assert.eq(ret.n, 2);

    let mongosServerStatus =
        assert.commandWorked(mongos.getDB(jsTestName()).adminCommand({serverStatus: 1}));
    assert.eq(numOps, mongosServerStatus.metrics.query.updateOneNonTargetedShardedCount);
    assert.eq(numOps, mongosServerStatus.metrics.query.deleteOneWithoutShardKeyWithIdCount);
    session.endSession();
};

// Test batched ops with ordered: true and ordered: false.
performOps(true, 2);
performOps(false, 4);

st.stop();
