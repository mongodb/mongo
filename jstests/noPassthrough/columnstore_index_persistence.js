/**
 * Tests around columnstore indexes and persistence. In particular, this tests that a columnstore
 * index can be persisted, appears in listIndexes, and that a warning is added to the startup log
 * as well as the createIndex response when a columnstore index is created.
 *
 * @tags: [
 *   requires_fcv_63,
 *   requires_persistence,
 *   requires_replication,
 * ]
 */

(function() {
'use strict';

load('jstests/libs/index_catalog_helpers.js');
load("jstests/libs/columnstore_util.js");  // For setUpServerForColumnStoreIndexTest.

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

let primary = rst.getPrimary();

const collName = 'columnstore_index_persistence';
let db_primary = primary.getDB('test');

if (!setUpServerForColumnStoreIndexTest(db_primary)) {
    rst.stopSet();
    return;
}

let coll_primary = db_primary.getCollection(collName);
// Create the collection by inserting a dummy doc.
assert.commandWorked(coll_primary.insert({a: 1}));

const previewFeatureRegex = /preview feature/;

{
    // Create the index, and check that the command returns a note indicating that this feature is
    // in preview.
    const createResponse = assert.commandWorked(coll_primary.createIndex({"$**": "columnstore"}));
    assert(previewFeatureRegex.test(createResponse.note), createResponse);
}

// Restart the primary and run some checks.
rst.restart(primary);
rst.waitForPrimary();

// Reset our handles after restarting the primary node.
primary = rst.getPrimary();
db_primary = primary.getDB('test');
coll_primary = db_primary.getCollection(collName);

{
    // Test that the code for the CSI preview warning appears in the startup log.
    const getLogRes = assert.commandWorked(db_primary.adminCommand({getLog: "startupWarnings"}));
    assert(/7281100/.test(getLogRes.log), getLogRes.log);
}

{
    // Check that attempting to recreate the index still reports the "preview feature" note.
    const createAgainResponse =
        assert.commandWorked(coll_primary.createIndex({"$**": "columnstore"}));
    assert.eq(createAgainResponse.numIndexesBefore, createAgainResponse.numIndexesAfter);
    assert(previewFeatureRegex.test(createAgainResponse.note), createAgainResponse);
}

{
    // Check that the index appears in the listIndex output.
    const indexList = db_primary.getCollection(collName).getIndexes();
    assert.neq(null, IndexCatalogHelpers.findByKeyPattern(indexList, {"$**": "columnstore"}));
}

rst.stopSet();
})();
