/**
 * Tests that db.collection.stats().freeStorageSize is a non-zero value after records are
 * inserted and then some deleted.
 *
 * @tags: [
 *   # freeStorageSize is currently only supported by WT.
 *   requires_wiredtiger,
 *   # inMemory will not have the freeStorageSize field.
 *   requires_persistence
 * ]
 */

(function() {
"use strict";

const forceCheckpoint = () => {
    assert.commandWorked(db.fsyncLock());
    assert.commandWorked(db.fsyncUnlock());
};

const dbName = "freeStorageSizeTest";
const collName = "foo";
const testDB = db.getSiblingDB(dbName);
const coll = testDB.getCollection(collName);

const kDocsToInsert = 150;
const kDocsToRemove = kDocsToInsert / 2;

// Insert docs.
for (let i = 0; i < kDocsToInsert; i++) {
    assert.commandWorked(coll.insert({a: i}));
}

forceCheckpoint();

// Remove docs to free up space.
assert.commandWorked(coll.remove({a: {$lt: kDocsToRemove}}));

forceCheckpoint();

// Check that freeStorageSize is non-zero.
let collStats = assert.commandWorked(testDB.runCommand({collStats: collName}));
assert(collStats.hasOwnProperty("freeStorageSize"), tojson(collStats));
assert.gt(collStats.freeStorageSize, 0);
})();
