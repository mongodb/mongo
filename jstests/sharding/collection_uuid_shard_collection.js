/**
 * Tests the collectionUUID parameter of the shardCollection command.
 *
 * @tags: [
 *   featureFlagCommandsAcceptCollectionUUID,
 * ]
 */
(function() {
'use strict';

const st = new ShardingTest({shards: 2});
const mongos = st.s0;

const db = mongos.getDB(jsTestName());
assert.commandWorked(mongos.adminCommand({enableSharding: db.getName()}));

const coll = db['coll'];

const resetColl = function() {
    coll.drop();
    assert.commandWorked(coll.insert({_id: 0}));
};

const uuid = function() {
    return assert.commandWorked(db.runCommand({listCollections: 1}))
        .cursor.firstBatch.find(c => c.name === coll.getName())
        .info.uuid;
};

resetColl();

// The command succeeds when the correct UUID is provided.
assert.commandWorked(mongos.adminCommand(
    {shardCollection: coll.getFullName(), key: {_id: 1}, collectionUUID: uuid()}));

// The command fails when the provided UUID does not correspond to an existing collection.
resetColl();
const nonexistentUUID = UUID();
let res = assert.commandFailedWithCode(
    mongos.adminCommand(
        {shardCollection: coll.getFullName(), key: {_id: 1}, collectionUUID: nonexistentUUID}),
    ErrorCodes.CollectionUUIDMismatch);
assert.eq(res.collectionUUID, nonexistentUUID);
assert.eq(res.expectedNamespace, coll.getFullName());
assert.eq(res.actualNamespace, null);

// The command fails when the provided UUID corresponds to a different collection.
const coll2 = db['coll_2'];
assert.commandWorked(coll2.insert({_id: 1}));
res = assert.commandFailedWithCode(
    mongos.adminCommand(
        {shardCollection: coll2.getFullName(), key: {_id: 1}, collectionUUID: uuid()}),
    ErrorCodes.CollectionUUIDMismatch);
assert.eq(res.collectionUUID, uuid());
assert.eq(res.expectedNamespace, coll2.getFullName());
assert.eq(res.actualNamespace, coll.getFullName());

// The command fails when the collection is already sharded and the provided UUID corresponds to a
// different collection.
assert.commandWorked(mongos.adminCommand({shardCollection: coll2.getFullName(), key: {_id: 1}}));
res = assert.commandFailedWithCode(
    mongos.adminCommand(
        {shardCollection: coll2.getFullName(), key: {_id: 1}, collectionUUID: uuid()}),
    ErrorCodes.CollectionUUIDMismatch);
assert.eq(res.collectionUUID, uuid());
assert.eq(res.expectedNamespace, coll2.getFullName());
assert.eq(res.actualNamespace, coll.getFullName());

// The command fails when the provided UUID corresponds to a different collection, even if the
// provided namespace does not exist.
coll2.drop();
res = assert.commandFailedWithCode(
    mongos.adminCommand(
        {shardCollection: coll2.getFullName(), key: {_id: 1}, collectionUUID: uuid()}),
    ErrorCodes.CollectionUUIDMismatch);
assert.eq(res.collectionUUID, uuid());
assert.eq(res.expectedNamespace, coll2.getFullName());
assert.eq(res.actualNamespace, coll.getFullName());

st.stop();
})();
