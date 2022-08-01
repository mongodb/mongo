/**
 * Tests the collectionUUID parameter of the shardCollection command against capped collections.
 */
(function() {
'use strict';

const st = new ShardingTest({shards: 1});
const mongos = st.s0;

const db = mongos.getDB(jsTestName());
const coll = db['cappedColl'];
const collName = coll.getName();

// Create a capped collection.
assert.commandWorked(db.createCollection(collName, {capped: true, size: 1024}));
assert.commandWorked(mongos.adminCommand({enableSharding: db.getName()}));

// Ensure that we fail the shardCollection command with 'CollectionUUIDMismatch' when the UUID does
// not correspond to the collection.
const nonexistentUUID = UUID();
let res = assert.commandFailedWithCode(
    mongos.adminCommand(
        {shardCollection: coll.getFullName(), key: {_id: 1}, collectionUUID: nonexistentUUID}),
    ErrorCodes.CollectionUUIDMismatch);
assert.eq(res.db, db.getName());
assert.eq(res.collectionUUID, nonexistentUUID);
assert.eq(res.expectedCollection, collName);
assert.eq(res.actualCollection, null);

const uuid = assert.commandWorked(db.runCommand({listCollections: 1}))
                 .cursor.firstBatch.find(c => c.name === collName)
                 .info.uuid;

// Ensure that we fail the shard command with 'InvalidOptions' when the UUID corresponds to the
// capped collection.
assert.commandFailedWithCode(
    mongos.adminCommand({shardCollection: coll.getFullName(), collectionUUID: uuid, key: {_id: 1}}),
    ErrorCodes.InvalidOptions);

st.stop();
})();
