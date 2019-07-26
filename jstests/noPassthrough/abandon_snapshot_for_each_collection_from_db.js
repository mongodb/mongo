/**
 * The 'forEachCollectionFromDb' CollectionCatalog helper should abandon its snapshot before it
 * performs the user provided callback on each collection.
 * Any newly added collections have the chance to be seen as 'forEachCollectionFromDb' iterates over
 * the remaining collections. This could lead to inconsistencies, such as seeing indexes in the
 * IndexCatalog of the new collection but not seeing them on-disk due to using an older snapshot
 * (BF-13133).
 *
 * @tags: [requires_replication]
 */
(function() {
"use strict";

const dbName = "test";
const collName = "coll";

const rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();

const db = rst.getPrimary().getDB(dbName);
assert.commandWorked(db.createCollection(collName));

const failpoint = 'hangBeforeGettingNextCollection';

// Hang 'forEachCollectionFromDb' after iterating through the first collection.
assert.commandWorked(db.adminCommand({configureFailPoint: failpoint, mode: "alwaysOn"}));

TestData.failpoint = failpoint;
const awaitCreateCollections = startParallelShell(() => {
    // The 'forEachCollectionFromDb' helper doesn't iterate in collection name order, so we need
    // to insert multiple collections to have at least one next collection when the
    // CollectionCatalog iterator is incremented.
    for (let i = 0; i < 25; i++) {
        const collName = "a".repeat(i + 1);
        assert.commandWorked(db.createCollection(collName));
    }

    // Let 'forEachCollectionFromDb' iterate to the next collection.
    assert.commandWorked(db.adminCommand({configureFailPoint: TestData.failpoint, mode: "off"}));
}, rst.getPrimary().port);

assert.commandWorked(db.stats());
awaitCreateCollections();

rst.stopSet();
}());
