/**
 * Tests the collectionUUID parameter of the collMod command.
 *
 * @tags: [
 *   requires_fcv_60,
 *   tenant_migration_incompatible,
 * ]
 */
(function() {
'use strict';

const testDB = db.getSiblingDB(jsTestName());
assert.commandWorked(testDB.dropDatabase());

const coll = testDB['coll'];
assert.commandWorked(coll.insert({_id: 0}));

const uuid =
    assert.commandWorked(testDB.runCommand({listCollections: 1})).cursor.firstBatch[0].info.uuid;

// 1. The command succeeds when the correct UUID is provided.
assert.commandWorked(testDB.runCommand({collMod: coll.getName(), collectionUUID: uuid}));

// 2. The command fails when the provided UUID does not correspond to an existing collection.
const nonexistentUUID = UUID();
let res = assert.commandFailedWithCode(
    testDB.runCommand({collMod: coll.getName(), collectionUUID: nonexistentUUID}),
    ErrorCodes.CollectionUUIDMismatch);
assert.eq(res.db, testDB.getName());
assert.eq(res.collectionUUID, nonexistentUUID);
assert.eq(res.expectedCollection, coll.getName());
assert.eq(res.actualCollection, null);

// 3. The command fails when the provided UUID corresponds to a different collection.
const coll2 = testDB['coll_2'];
assert.commandWorked(coll2.insert({_id: 1}));
res = assert.commandFailedWithCode(
    testDB.runCommand({collMod: coll2.getName(), collectionUUID: uuid}),
    ErrorCodes.CollectionUUIDMismatch);
assert.eq(res.db, testDB.getName());
assert.eq(res.collectionUUID, uuid);
assert.eq(res.expectedCollection, coll2.getName());
assert.eq(res.actualCollection, coll.getName());

// 4. Only collections in the same database are specified by actualCollection.
const otherDB = testDB.getSiblingDB(testDB.getName() + '_2');
assert.commandWorked(otherDB.dropDatabase());
const coll3 = otherDB['coll_3'];
assert.commandWorked(coll3.insert({_id: 2}));
res = assert.commandFailedWithCode(
    otherDB.runCommand({collMod: coll3.getName(), collectionUUID: uuid}),
    ErrorCodes.CollectionUUIDMismatch);
assert.eq(res.db, otherDB.getName());
assert.eq(res.collectionUUID, uuid);
assert.eq(res.expectedCollection, coll3.getName());
assert.eq(res.actualCollection, null);

// 5. The command fails when the provided UUID corresponds to a different collection, even if the
// provided namespace does not exist.
assert.commandWorked(testDB.runCommand({drop: coll2.getName()}));
res = assert.commandFailedWithCode(
    testDB.runCommand({collMod: coll2.getName(), collectionUUID: uuid}),
    ErrorCodes.CollectionUUIDMismatch);
assert.eq(res.db, testDB.getName());
assert.eq(res.collectionUUID, uuid);
assert.eq(res.expectedCollection, coll2.getName());
assert.eq(res.actualCollection, coll.getName());
assert(!testDB.getCollectionNames().includes(coll2.getName()));

// 6. The command fails with CollectionUUIDMismatch even if the database does not exist.
const nonexistentDB = testDB.getSiblingDB(testDB.getName() + '_nonexistent');
res = assert.commandFailedWithCode(
    nonexistentDB.runCommand({collMod: 'nonexistent', collectionUUID: uuid}),
    ErrorCodes.CollectionUUIDMismatch);
assert.eq(res.db, nonexistentDB.getName());
assert.eq(res.collectionUUID, uuid);
assert.eq(res.expectedCollection, 'nonexistent');
assert.eq(res.actualCollection, null);
})();
