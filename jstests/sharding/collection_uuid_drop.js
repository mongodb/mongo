/**
 * Tests the collectionUUID parameter of the drop command when one collection is sharded and the
 * other collection is unsharded.
 *
 * @tags: [
 *   requires_fcv_60,
 * ]
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 2});
const mongos = st.s;

const db = mongos.getDB(jsTestName());
assert.commandWorked(mongos.adminCommand({enableSharding: db.getName(), primaryShard: st.shard0.shardName}));

const shardedColl = db.sharded;
const unshardedColl = db.unsharded;

assert.commandWorked(shardedColl.insert({_id: 0}));
assert.commandWorked(shardedColl.insert({_id: 1}));
assert.commandWorked(unshardedColl.insert({_id: 2}));

const uuid = function (coll) {
    return assert
        .commandWorked(db.runCommand({listCollections: 1}))
        .cursor.firstBatch.find((c) => c.name === coll.getName()).info.uuid;
};

assert.commandWorked(mongos.adminCommand({shardCollection: shardedColl.getFullName(), key: {_id: 1}}));

// Move {_id: 0} to shard0 and {_id: 1} to shard1.
assert.commandWorked(st.splitAt(shardedColl.getFullName(), {_id: 1}));
assert.commandWorked(
    mongos.adminCommand({moveChunk: shardedColl.getFullName(), find: {_id: 0}, to: st.shard0.shardName}),
);
assert.commandWorked(
    mongos.adminCommand({moveChunk: shardedColl.getFullName(), find: {_id: 1}, to: st.shard1.shardName}),
);

// Run the drop command on the collection that is sharded, while specifying the UUID of the
// unsharded collection which only exists on shard0.
let res = assert.commandFailedWithCode(
    db.runCommand({drop: shardedColl.getName(), collectionUUID: uuid(unshardedColl)}),
    ErrorCodes.CollectionUUIDMismatch,
);
assert.eq(res.db, db.getName());
assert.eq(res.collectionUUID, uuid(unshardedColl));
assert.eq(res.expectedCollection, shardedColl.getName());
assert.eq(res.actualCollection, unshardedColl.getName());

// Run the drop command on the unsharded collection, which only exists on shard0.
res = assert.commandFailedWithCode(
    db.runCommand({drop: unshardedColl.getName(), collectionUUID: uuid(shardedColl)}),
    ErrorCodes.CollectionUUIDMismatch,
);
assert.eq(res.db, db.getName());
assert.eq(res.collectionUUID, uuid(shardedColl));
assert.eq(res.expectedCollection, unshardedColl.getName());
assert.eq(res.actualCollection, shardedColl.getName());

st.stop();
