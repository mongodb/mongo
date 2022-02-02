/**
 * Tests the collectionUUID parameter of the renameCollection command.
 *
 * @tags: [
 *   does_not_support_zones,
 *   featureFlagCommandsAcceptCollectionUUID,
 *   no_selinux,
 *   requires_non_retryable_commands,
 *   tenant_migration_incompatible,
 * ]
 */
(function() {
'use strict';

const testDB = db.getSiblingDB(jsTestName());
assert.commandWorked(testDB.dropDatabase());

const coll = testDB.coll;
const coll2 = testDB.coll_2;
const coll3 = testDB.coll_3;

const resetColls = function() {
    coll.drop();
    coll2.drop();
    coll3.drop();

    assert.commandWorked(coll.insert({_id: 0}));
    assert.commandWorked(coll2.insert({_id: 1}));
};

const uuid = function(coll) {
    return assert.commandWorked(testDB.runCommand({listCollections: 1}))
        .cursor.firstBatch.find(c => c.name === coll.getName())
        .info.uuid;
};

// The command succeeds when the correct UUID is provided.
resetColls();
assert.commandWorked(testDB.adminCommand({
    renameCollection: coll.getFullName(),
    to: coll3.getFullName(),
    dropTarget: true,
    collectionUUID: uuid(coll),
}));

resetColls();
assert.commandWorked(testDB.adminCommand({
    renameCollection: coll2.getFullName(),
    to: coll.getFullName(),
    dropTarget: uuid(coll),
}));

resetColls();
assert.commandWorked(testDB.adminCommand({
    renameCollection: coll2.getFullName(),
    to: coll.getFullName(),
    dropTarget: uuid(coll),
    collectionUUID: uuid(coll2),
}));

// The command fails when the provided UUID does not correspond to an existing collection.
resetColls();
const nonexistentUUID = UUID();
let res = assert.commandFailedWithCode(testDB.adminCommand({
    renameCollection: coll.getFullName(),
    to: coll3.getFullName(),
    dropTarget: true,
    collectionUUID: nonexistentUUID,
}),
                                       ErrorCodes.CollectionUUIDMismatch);
assert.eq(res.collectionUUID, nonexistentUUID);
assert.eq(res.actualNamespace, "");

res = assert.commandFailedWithCode(testDB.adminCommand({
    renameCollection: coll2.getFullName(),
    to: coll.getFullName(),
    dropTarget: nonexistentUUID,
}),
                                   ErrorCodes.CollectionUUIDMismatch);
assert.eq(res.collectionUUID, nonexistentUUID);
assert.eq(res.actualNamespace, "");

// The command fails when the provided UUID corresponds to a different collection.
res = assert.commandFailedWithCode(testDB.adminCommand({
    renameCollection: coll2.getFullName(),
    to: coll3.getFullName(),
    dropTarget: true,
    collectionUUID: uuid(coll),
}),
                                   ErrorCodes.CollectionUUIDMismatch);
assert.eq(res.collectionUUID, uuid(coll));
assert.eq(res.actualNamespace, coll.getFullName());

res = assert.commandFailedWithCode(testDB.adminCommand({
    renameCollection: coll.getFullName(),
    to: coll2.getFullName(),
    dropTarget: uuid(coll),
}),
                                   ErrorCodes.CollectionUUIDMismatch);
assert.eq(res.collectionUUID, uuid(coll));
assert.eq(res.actualNamespace, coll.getFullName());

res = assert.commandFailedWithCode(testDB.adminCommand({
    renameCollection: coll2.getFullName(),
    to: coll3.getFullName(),
    dropTarget: uuid(coll2),
    collectionUUID: uuid(coll),
}),
                                   ErrorCodes.CollectionUUIDMismatch);
assert.eq(res.collectionUUID, uuid(coll));
assert.eq(res.actualNamespace, coll.getFullName());

// The collectionUUID parameter cannot be provided when renaming a collection between databases.
const otherDBColl = db.getSiblingDB(jsTestName() + '_2').coll;
otherDBColl.drop();
assert.commandWorked(otherDBColl.insert({_id: 3}));
assert.commandFailedWithCode(testDB.adminCommand({
    renameCollection: coll.getFullName(),
    to: otherDBColl.getFullName(),
    dropTarget: true,
    collectionUUID: uuid(coll),
}),
                             [ErrorCodes.InvalidOptions, ErrorCodes.CommandFailed]);

assert.commandFailedWithCode(testDB.adminCommand({
    renameCollection: coll.getFullName(),
    to: otherDBColl.getFullName(),
    dropTarget: uuid(coll),
}),
                             [ErrorCodes.InvalidOptions, ErrorCodes.CommandFailed]);
})();
