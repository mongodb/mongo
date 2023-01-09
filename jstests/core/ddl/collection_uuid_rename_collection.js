/**
 * Tests the collectionUUID parameter of the renameCollection command.
 *
 * @tags: [
 *   does_not_support_zones,
 *   requires_fcv_60,
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
assert.eq(res.db, testDB.getName());
assert.eq(res.collectionUUID, nonexistentUUID);
assert.eq(res.expectedCollection, coll.getName());
assert.eq(res.actualCollection, null);

res = assert.commandFailedWithCode(testDB.adminCommand({
    renameCollection: coll2.getFullName(),
    to: coll.getFullName(),
    dropTarget: nonexistentUUID,
}),
                                   ErrorCodes.CollectionUUIDMismatch);
assert.eq(res.db, testDB.getName());
assert.eq(res.collectionUUID, nonexistentUUID);
assert.eq(res.expectedCollection, coll.getName());
assert.eq(res.actualCollection, null);

// The command fails when the provided UUID corresponds to a different collection.
res = assert.commandFailedWithCode(testDB.adminCommand({
    renameCollection: coll2.getFullName(),
    to: coll3.getFullName(),
    dropTarget: true,
    collectionUUID: uuid(coll),
}),
                                   ErrorCodes.CollectionUUIDMismatch);
assert.eq(res.db, testDB.getName());
assert.eq(res.collectionUUID, uuid(coll));
assert.eq(res.expectedCollection, coll2.getName());
assert.eq(res.actualCollection, coll.getName());

res = assert.commandFailedWithCode(testDB.adminCommand({
    renameCollection: coll.getFullName(),
    to: coll2.getFullName(),
    dropTarget: uuid(coll),
}),
                                   ErrorCodes.CollectionUUIDMismatch);
assert.eq(res.db, testDB.getName());
assert.eq(res.collectionUUID, uuid(coll));
assert.eq(res.expectedCollection, coll2.getName());
assert.eq(res.actualCollection, coll.getName());

res = assert.commandFailedWithCode(testDB.adminCommand({
    renameCollection: coll2.getFullName(),
    to: coll3.getFullName(),
    dropTarget: uuid(coll2),
    collectionUUID: uuid(coll),
}),
                                   ErrorCodes.CollectionUUIDMismatch);
assert.eq(res.db, testDB.getName());
assert.eq(res.collectionUUID, uuid(coll));
assert.eq(res.expectedCollection, coll2.getName());
assert.eq(res.actualCollection, coll.getName());

// Only collections in the same database are specified by actualCollection.
const otherDB = testDB.getSiblingDB(testDB.getName() + '_2');
assert.commandWorked(otherDB.dropDatabase());
const coll4 = otherDB['coll_4'];
const coll5 = otherDB['coll_5'];
assert.commandWorked(coll4.insert({_id: 2}));
res = assert.commandFailedWithCode(otherDB.adminCommand({
    renameCollection: coll4.getFullName(),
    to: coll5.getFullName(),
    dropTarget: true,
    collectionUUID: uuid(coll),
}),
                                   ErrorCodes.CollectionUUIDMismatch);
assert.eq(res.db, otherDB.getName());
assert.eq(res.collectionUUID, uuid(coll));
assert.eq(res.expectedCollection, coll4.getName());
assert.eq(res.actualCollection, null);

res = assert.commandFailedWithCode(otherDB.adminCommand({
    renameCollection: coll4.getFullName(),
    to: coll5.getFullName(),
    dropTarget: uuid(coll),
}),
                                   ErrorCodes.CollectionUUIDMismatch);
assert.eq(res.db, otherDB.getName());
assert.eq(res.collectionUUID, uuid(coll));
assert.eq(res.expectedCollection, coll5.getName());
assert.eq(res.actualCollection, null);

res = assert.commandFailedWithCode(otherDB.adminCommand({
    renameCollection: coll4.getFullName(),
    to: coll5.getFullName(),
    dropTarget: uuid(coll2),
    collectionUUID: uuid(coll),
}),
                                   ErrorCodes.CollectionUUIDMismatch);
assert.eq(res.db, otherDB.getName());
assert.eq(res.collectionUUID, uuid(coll));
assert.eq(res.expectedCollection, coll4.getName());
assert.eq(res.actualCollection, null);

// The command fails when the provided UUID corresponds to a different collection, even if the
// provided source namespace does not exist.
assert.commandWorked(testDB.runCommand({drop: coll2.getName()}));
res = assert.commandFailedWithCode(testDB.adminCommand({
    renameCollection: coll2.getFullName(),
    to: coll3.getFullName(),
    dropTarget: true,
    collectionUUID: uuid(coll),
}),
                                   ErrorCodes.CollectionUUIDMismatch);
assert.eq(res.db, testDB.getName());
assert.eq(res.collectionUUID, uuid(coll));
assert.eq(res.expectedCollection, coll2.getName());
assert.eq(res.actualCollection, coll.getName());
assert(!testDB.getCollectionNames().includes(coll2.getName()));

// The command fails with CollectionUUIDMismatch even if the database does not exist.
const nonexistentDB = testDB.getSiblingDB(testDB.getName() + '_nonexistent');
res = assert.commandFailedWithCode(testDB.adminCommand({
    renameCollection: nonexistentDB.getName() + '.nonexistent',
    to: nonexistentDB.getName() + '.nonexistent_2',
    dropTarget: true,
    collectionUUID: uuid(coll),
}),
                                   ErrorCodes.CollectionUUIDMismatch);
assert.eq(res.db, nonexistentDB.getName());
assert.eq(res.collectionUUID, uuid(coll));
assert.eq(res.expectedCollection, 'nonexistent');
assert.eq(res.actualCollection, null);

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
