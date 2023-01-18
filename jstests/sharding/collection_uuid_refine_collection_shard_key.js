/**
 * Tests the collectionUUID parameter of the refineCollectionShardKey command.
 *
 * @tags: [
 *   requires_fcv_60,
 * ]
 */

// Cannot run the filtering metadata check on tests that run refineCollectionShardKey.
TestData.skipCheckShardFilteringMetadata = true;

(function() {
'use strict';

const st = new ShardingTest({shards: 1});
const mongos = st.s0;
const db = mongos.getDB(jsTestName());
const coll = db['coll'];
assert.commandWorked(mongos.adminCommand({enableSharding: db.getName()}));

const oldKeyDoc = {
    _id: 1,
    a: 1
};
const newKeyDoc = {
    _id: 1,
    a: 1,
    b: 1,
    c: 1
};

const resetColl = function(shardedColl) {
    shardedColl.drop();
    assert.commandWorked(shardedColl.insert({_id: 0, a: 1, b: 2, c: 3}));
    assert.commandWorked(mongos.getCollection(shardedColl.getFullName()).createIndex(newKeyDoc));
    assert.commandWorked(
        mongos.adminCommand({shardCollection: shardedColl.getFullName(), key: oldKeyDoc}));
};

const uuid = function() {
    return assert.commandWorked(db.runCommand({listCollections: 1}))
        .cursor.firstBatch.find(c => c.name === coll.getName())
        .info.uuid;
};

resetColl(coll);

// The command succeeds when provided with the correct collection UUID.
assert.commandWorked(mongos.adminCommand(
    {refineCollectionShardKey: coll.getFullName(), key: newKeyDoc, collectionUUID: uuid()}));

// The command fails when provided with a UUID with no corresponding collection.
resetColl(coll);
const nonexistentUUID = UUID();
let res = assert.commandFailedWithCode(mongos.adminCommand({
    refineCollectionShardKey: coll.getFullName(),
    key: newKeyDoc,
    collectionUUID: nonexistentUUID,
}),
                                       ErrorCodes.CollectionUUIDMismatch);
assert.eq(res.db, db.getName());
assert.eq(res.collectionUUID, nonexistentUUID);
assert.eq(res.expectedCollection, coll.getName());
assert.eq(res.actualCollection, null);

// The command fails when provided with a different collection's UUID.
const coll2 = db['coll_2'];
resetColl(coll2);
res = assert.commandFailedWithCode(mongos.adminCommand({
    refineCollectionShardKey: coll2.getFullName(),
    key: newKeyDoc,
    collectionUUID: uuid(),
}),
                                   ErrorCodes.CollectionUUIDMismatch);
assert.eq(res.db, db.getName());
assert.eq(res.collectionUUID, uuid());
assert.eq(res.expectedCollection, coll2.getName());
assert.eq(res.actualCollection, coll.getName());

// Only collections in the same database are specified by actualCollection.
const otherDB = db.getSiblingDB(db.getName() + '_2');
const coll3 = otherDB['coll_3'];
assert.commandWorked(mongos.adminCommand({enableSharding: otherDB.getName()}));
resetColl(coll3);
res = assert.commandFailedWithCode(mongos.adminCommand({
    refineCollectionShardKey: coll3.getFullName(),
    key: newKeyDoc,
    collectionUUID: uuid(),
}),
                                   ErrorCodes.CollectionUUIDMismatch);
assert.eq(res.db, otherDB.getName());
assert.eq(res.collectionUUID, uuid());
assert.eq(res.expectedCollection, coll3.getName());
assert.eq(res.actualCollection, null);

// The command fails when provided with a different collection's UUID, even if the provided
// namespace does not exist.
coll2.drop();
res = assert.commandFailedWithCode(mongos.adminCommand({
    refineCollectionShardKey: coll2.getFullName(),
    key: newKeyDoc,
    collectionUUID: uuid(),
}),
                                   ErrorCodes.CollectionUUIDMismatch);
assert.eq(res.db, db.getName());
assert.eq(res.collectionUUID, uuid());
assert.eq(res.expectedCollection, coll2.getName());
assert.eq(res.actualCollection, coll.getName());

st.stop();
})();
