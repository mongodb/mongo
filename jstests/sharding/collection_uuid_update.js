/**
 * Tests the collectionUUID parameter of the update command when one collection is sharded and the
 * other collection is unsharded.
 *
 * @tags: [
 *   requires_fcv_60,
 * ]
 */
(function() {
'use strict';

const st = new ShardingTest({shards: 2});
const mongos = st.s;

const db = mongos.getDB(jsTestName());
assert.commandWorked(mongos.adminCommand({enableSharding: db.getName()}));
st.ensurePrimaryShard(db.getName(), st.shard0.shardName);

const shardedColl = db.sharded;
const unshardedColl = db.unsharded;

assert.commandWorked(shardedColl.insert([{_id: 0}, {_id: 1}, {_id: 2}, {_id: 3}]));
assert.commandWorked(unshardedColl.insert([{_id: 0}, {_id: 1}]));

const uuid = function(coll) {
    return assert.commandWorked(db.runCommand({listCollections: 1}))
        .cursor.firstBatch.find(c => c.name === coll.getName())
        .info.uuid;
};

assert.commandWorked(
    mongos.adminCommand({shardCollection: shardedColl.getFullName(), key: {_id: 1}}));

// Move {_id: 0} to shard0 and {_id: 1} to shard1.
assert.commandWorked(st.splitAt(shardedColl.getFullName(), {_id: 1}));
assert.commandWorked(mongos.adminCommand(
    {moveChunk: shardedColl.getFullName(), find: {_id: 0}, to: st.shard0.shardName}));
assert.commandWorked(mongos.adminCommand(
    {moveChunk: shardedColl.getFullName(), find: {_id: 1}, to: st.shard1.shardName}));

// Run an update which only targets shard1, while the unsharded collection only exists on shard0.
let res = assert
              .commandFailedWithCode(db.runCommand({
                  update: shardedColl.getName(),
                  updates: [{q: {_id: 1}, u: {}}],
                  collectionUUID: uuid(unshardedColl),
              }),
                                     ErrorCodes.CollectionUUIDMismatch)
              .writeErrors[0];
assert.eq(res.db, db.getName());
assert.eq(res.collectionUUID, uuid(unshardedColl));
assert.eq(res.expectedCollection, shardedColl.getName());
assert.eq(res.actualCollection, unshardedColl.getName());

// Run an update on the sharded collection with multiple updates that only targets shard1.
res = assert
          .commandFailedWithCode(db.runCommand({
              update: shardedColl.getName(),
              updates: [{q: {_id: 1}, u: {}}, {q: {_id: 2}, u: {}}, {q: {_id: 3}, u: {}}],
              ordered: false,
              collectionUUID: uuid(unshardedColl),
          }),
                                 ErrorCodes.CollectionUUIDMismatch)
          .writeErrors;
for (let writeError of res) {
    assert.eq(writeError.db, db.getName());
    assert.eq(writeError.collectionUUID, uuid(unshardedColl));
    assert.eq(writeError.expectedCollection, shardedColl.getName());
    assert.eq(writeError.actualCollection, unshardedColl.getName());
}

// Run an update on the sharded collection with multiple updates that targets both shards.
res = assert
          .commandFailedWithCode(db.runCommand({
              update: shardedColl.getName(),
              updates: [{q: {_id: 1}, u: {}}, {q: {_id: 2}, u: {}}, {q: {_id: 0}, u: {}}],
              ordered: false,
              collectionUUID: uuid(unshardedColl),
          }),
                                 ErrorCodes.CollectionUUIDMismatch)
          .writeErrors;
for (let writeError of res) {
    assert.eq(writeError.db, db.getName());
    assert.eq(writeError.collectionUUID, uuid(unshardedColl));
    assert.eq(writeError.expectedCollection, shardedColl.getName());
    assert.eq(writeError.actualCollection, unshardedColl.getName());
}

// Run an update on the unsharded collection, which only exists on shard0.
res = assert
          .commandFailedWithCode(db.runCommand({
              update: unshardedColl.getName(),
              updates: [{q: {_id: 2}, u: {}}],
              collectionUUID: uuid(shardedColl),
          }),
                                 ErrorCodes.CollectionUUIDMismatch)
          .writeErrors[0];
assert.eq(res.db, db.getName());
assert.eq(res.collectionUUID, uuid(shardedColl));
assert.eq(res.expectedCollection, unshardedColl.getName());
assert.eq(res.actualCollection, shardedColl.getName());

// Run an update on the unsharded collection with multiple updates.
res = assert
          .commandFailedWithCode(db.runCommand({
              update: unshardedColl.getName(),
              updates: [{q: {_id: 0}, u: {}}, {q: {_id: 1}, u: {}}],
              ordered: false,
              collectionUUID: uuid(shardedColl),
          }),
                                 ErrorCodes.CollectionUUIDMismatch)
          .writeErrors;
for (let writeError of res) {
    assert.eq(writeError.db, db.getName());
    assert.eq(writeError.collectionUUID, uuid(shardedColl));
    assert.eq(writeError.expectedCollection, unshardedColl.getName());
    assert.eq(writeError.actualCollection, shardedColl.getName());
}

st.stop();
})();
