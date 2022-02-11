/**
 * Tests the collectionUUID parameter of the find command.
 *
 * @tags: [
 *   featureFlagCommandsAcceptCollectionUUID,
 *   tenant_migration_incompatible,
 *   no_selinux,
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

// The command succeeds when the correct UUID is provided.
assert.commandWorked(testDB.runCommand({find: coll.getName(), collectionUUID: uuid}));

// The command fails when the provided UUID does not correspond to an existing collection.
const nonexistentUUID = UUID();
let res = assert.commandFailedWithCode(
    testDB.runCommand({find: coll.getName(), collectionUUID: nonexistentUUID}),
    ErrorCodes.CollectionUUIDMismatch);
assert.eq(res.collectionUUID, nonexistentUUID);
assert.eq(res.expectedNamespace, coll.getFullName());
assert.eq(res.actualNamespace, null);

// The command fails when the provided UUID corresponds to a different collection.
const coll2 = testDB['coll_2'];
assert.commandWorked(coll2.insert({_id: 1}));
res = assert.commandFailedWithCode(testDB.runCommand({find: coll2.getName(), collectionUUID: uuid}),
                                   ErrorCodes.CollectionUUIDMismatch);
assert.eq(res.collectionUUID, uuid);
assert.eq(res.expectedNamespace, coll2.getFullName());
assert.eq(res.actualNamespace, coll.getFullName());

// The command fails when the provided UUID corresponds to a different collection, even if the
// provided namespace does not exist.
coll2.drop();
res = assert.commandFailedWithCode(testDB.runCommand({find: coll2.getName(), collectionUUID: uuid}),
                                   ErrorCodes.CollectionUUIDMismatch);
assert.eq(res.collectionUUID, uuid);
assert.eq(res.expectedNamespace, coll2.getFullName());
assert.eq(res.actualNamespace, coll.getFullName());

// The command fails when the provided UUID corresponds to a different collection, even if the
// provided namespace is a view.
const viewName = 'view';
assert.commandWorked(testDB.createView(viewName, coll.getName(), []));
res = assert.commandFailedWithCode(testDB.runCommand({find: viewName, collectionUUID: uuid}),
                                   ErrorCodes.CollectionUUIDMismatch);
assert.eq(res.collectionUUID, uuid);
assert.eq(res.expectedNamespace, testDB.getName() + '.' + viewName);
assert.eq(res.actualNamespace, coll.getFullName());
})();
