/**
 * Tests the collectionUUID parameter of the drop command.
 *
 * @tags: [
 *   featureFlagCommandsAcceptCollectionUUID,
 *   tenant_migration_incompatible,
 *   no_selinux,
 *   requires_non_retryable_commands,
 * ]
 */
(function() {
'use strict';

const testDB = db.getSiblingDB(jsTestName());
const coll = testDB["coll"];
assert.commandWorked(testDB.dropDatabase());

const createCollection = function(coll) {
    assert.commandWorked(coll.insert({_id: 0}));
    const uuid = assert.commandWorked(testDB.runCommand({listCollections: 1}))
                     .cursor.firstBatch.find(c => c.name === coll.getName())
                     .info.uuid;
    return uuid;
};

// The command fails when the provided UUID does not correspond to an existing collection.
let uuid = createCollection(coll);
const nonexistentUUID = UUID();
let res = assert.commandFailedWithCode(
    testDB.runCommand({drop: coll.getName(), collectionUUID: nonexistentUUID}),
    ErrorCodes.CollectionUUIDMismatch);
assert.eq(res.collectionUUID, nonexistentUUID);
assert.eq(res.expectedNamespace, coll.getFullName());
assert.eq(res.actualNamespace, "");

// The command fails when the provided UUID corresponds to a different collection.
const coll2 = testDB['coll_2'];
assert.commandWorked(coll2.insert({_id: 1}));
res = assert.commandFailedWithCode(testDB.runCommand({drop: coll2.getName(), collectionUUID: uuid}),
                                   ErrorCodes.CollectionUUIDMismatch);
assert.eq(res.collectionUUID, uuid);
assert.eq(res.expectedNamespace, coll2.getFullName());
assert.eq(res.actualNamespace, coll.getFullName());

// The command fails when the provided UUID corresponds to a different collection, even if the
// provided namespace does not exist.
coll2.drop();
res = assert.commandFailedWithCode(testDB.runCommand({drop: coll2.getName(), collectionUUID: uuid}),
                                   ErrorCodes.CollectionUUIDMismatch);
assert.eq(res.collectionUUID, uuid);
assert.eq(res.expectedNamespace, coll2.getFullName());
assert.eq(res.actualNamespace, coll.getFullName());

// The command fails when the provided UUID corresponds to a different collection, even if the
// provided namespace is a view.
const view = 'view';
assert.commandWorked(testDB.createView(view, coll.getName(), []));
res = assert.commandFailedWithCode(testDB.runCommand({drop: view, collectionUUID: uuid}),
                                   ErrorCodes.CollectionUUIDMismatch);
assert.eq(res.collectionUUID, uuid);
assert.eq(res.expectedNamespace, testDB.getName() + "." + view);
assert.eq(res.actualNamespace, coll.getFullName());

// The command succeeds when the correct UUID is provided.
assert.commandWorked(testDB.runCommand({drop: coll.getName(), collectionUUID: uuid}));
})();
