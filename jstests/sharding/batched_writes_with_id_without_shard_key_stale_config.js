/**
 * Tests batched updateOnes & deleteOnes with id without shard key work with StaleConfigError.
 *
 * @tags: [
 *    requires_fcv_80,
 *    # TODO (SERVER-97257): Re-enable this test or add an explanation why it is incompatible.
 *    embedded_router_incompatible,
 * ]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";
import {CreateShardedCollectionUtil} from "jstests/sharding/libs/create_sharded_collection_util.js";

const st = new ShardingTest({shards: 2, mongos: 2});
const mongos = st.s0;
let db = mongos.getDB(jsTestName());

const coll = db.coll;
coll.drop();

assert.commandWorked(
    db.adminCommand({enableSharding: db.getName(), primaryShard: st.rs0.getURL()}));

CreateShardedCollectionUtil.shardCollectionWithChunks(coll, {x: 1}, [
    {min: {x: MinKey}, max: {x: -100}, shard: st.shard0.shardName},
    {min: {x: -100}, max: {x: 0}, shard: st.shard0.shardName},
    {min: {x: 0}, max: {x: MaxKey}, shard: st.shard1.shardName},
]);

const performOps = function(ordered, numRetryCount) {
    jsTest.log("Perform write with ordered: " + ordered);
    // Write two documents.
    assert.commandWorked(coll.insert({x: -1, _id: -1}));
    assert.commandWorked(coll.insert({x: 1, _id: 1}));

    assert.neq(st.s1.getDB(jsTestName()).coll.findOne({x: -1, _id: -1}));

    // Move chunk from shard0 to shard1.
    assert.commandWorked(
        db.adminCommand({moveChunk: coll.getFullName(), find: {x: -1}, to: st.shard1.shardName}));

    // This update via mongos1 should trigger a StaleConfigError as mongos1 is not aware of moved
    // chunk.
    const session = st.s1.startSession({retryWrites: true});
    const sessionColl = session.getDatabase(db.getName()).getCollection(db.coll.getName());
    let res = sessionColl.bulkWrite(
        [
            {updateOne: {"filter": {_id: -1}, "update": {$inc: {counter: 1}}}},
            {updateOne: {"filter": {_id: 1}, "update": {$inc: {counter: 1}}}},
            {updateOne: {"filter": {_id: 2}, "update": {$inc: {counter: 1}}}},
        ],
        {ordered: ordered});

    assert.eq(res.matchedCount, 2);
    let mongosServerStatus =
        assert.commandWorked(st.s1.getDB(jsTestName()).adminCommand({serverStatus: 1}));
    assert.eq(numRetryCount,
              mongosServerStatus.metrics.query.updateOneWithoutShardKeyWithIdRetryCount);
    assert.commandWorked(
        db.adminCommand({moveChunk: coll.getFullName(), find: {x: -1}, to: st.shard0.shardName}));

    res = sessionColl.bulkWrite(
        [
            {deleteOne: {"filter": {_id: -1}}},
            {deleteOne: {"filter": {_id: 1}}},
            {deleteOne: {"filter": {_id: 2}}},
        ],
        {ordered: ordered});
    assert.eq(res.deletedCount, 2);
    mongosServerStatus =
        assert.commandWorked(st.s1.getDB(jsTestName()).adminCommand({serverStatus: 1}));
    assert.eq(numRetryCount,
              mongosServerStatus.metrics.query.deleteOneWithoutShardKeyWithIdRetryCount);
    session.endSession();
};

// Test batched ops with ordered: true and ordered: false.
performOps(false, 3);
performOps(true, 4);

st.stop();
