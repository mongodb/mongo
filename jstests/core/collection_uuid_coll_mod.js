/**
 * Tests the collectionUUID parameter of the collMod command.
 *
 * @tags: [
 *   featureFlagCommandsAcceptCollectionUUID,
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
assert.eq(res.collectionUUID, nonexistentUUID);
assert.eq(res.actualNamespace, "");

// 3. The command fails when the provided UUID corresponds to a different collection.
const coll2 = testDB['coll_2'];
assert.commandWorked(coll2.insert({_id: 1}));
res = assert.commandFailedWithCode(
    testDB.runCommand({collMod: coll2.getName(), collectionUUID: uuid}),
    ErrorCodes.CollectionUUIDMismatch);
assert.eq(res.collectionUUID, uuid);
assert.eq(res.actualNamespace, coll.getFullName());
})();
