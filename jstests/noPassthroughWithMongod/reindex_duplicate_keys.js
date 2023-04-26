/**
 * Tests that reIndex command fails with duplicate key error when there are duplicates in the
 * collection.
 */

(function() {
"use strict";

const collNamePrefix = "reindex_duplicate_keys_";
let count = 0;

// Bypasses DuplicateKey insertion error for testing via failpoint.
let addDuplicateDocumentsToCol = function(db, coll, doc) {
    jsTestLog("Inserts documents without index entries.");
    assert.commandWorked(
        db.adminCommand({configureFailPoint: "skipIndexNewRecords", mode: "alwaysOn"}));

    assert.commandWorked(coll.insert(doc));
    assert.commandWorked(coll.insert(doc));

    assert.commandWorked(db.adminCommand({configureFailPoint: "skipIndexNewRecords", mode: "off"}));
};

let runTest = function(doc) {
    const collName = collNamePrefix + count++;
    const coll = db.getCollection(collName);
    coll.drop();

    // Makes sure to create the _id index.
    assert.commandWorked(db.createCollection(collName));
    if (doc) {
        assert.commandWorked(coll.createIndex(doc, {unique: true}));
    } else {
        doc = {_id: 1};
    }

    // Inserts two violating documents without indexing them.
    addDuplicateDocumentsToCol(db, coll, doc);

    // Checks reIndex command fails with duplicate key error.
    assert.commandFailedWithCode(coll.reIndex(), ErrorCodes.DuplicateKey);
};

runTest();
runTest({a: 1});
})();
