/**
 * Tests that building geo indexes using the hybrid method handles the unindexing of invalid
 * geo documents.
 *
 * @tags: [requires_document_locking, requires_replication]
 */
(function() {
'use strict';

load('jstests/noPassthrough/libs/index_build.js');

const rst = new ReplSetTest({
    nodes: [
        {},
        {
            // Disallow elections on secondary.
            rsConfig: {
                priority: 0,
                votes: 0,
            },
        },
    ]
});
const nodes = rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const testDB = primary.getDB('test');
const coll = testDB.getCollection('test');

assert.commandWorked(testDB.createCollection(coll.getName()));

// Insert an invalid geo document that will be removed before the indexer starts a collecton
// scan.
assert.commandWorked(coll.insert({
    _id: 0,
    b: {type: 'invalid_geo_json_type', coordinates: [100, 100]},
}));

// We are using this fail point to pause the index build before it starts the collection scan.
// This is important for this test because we are mutating the collection state before the index
// builder is able to observe the invalid geo document.
// By comparison, IndexBuildTest.pauseIndexBuilds() stalls the index build in the middle of the
// collection scan.
assert.commandWorked(
    testDB.adminCommand({configureFailPoint: 'hangAfterSettingUpIndexBuild', mode: 'alwaysOn'}));

const createIdx = IndexBuildTest.startIndexBuild(primary, coll.getFullName(), {b: '2dsphere'});
IndexBuildTest.waitForIndexBuildToScanCollection(testDB, coll.getName(), 'b_2dsphere');

// Insert a valid geo document to initialize the hybrid index builder's side table state.
assert.commandWorked(coll.insert({
    b: {type: 'Polygon', coordinates: [[[0, 0], [0, 1], [1, 1], [1, 0], [0, 0]]]},
}));

// Removing the invalid geo document should not cause any issues for the side table accounting.
assert.commandWorked(coll.remove({_id: 0}));

assert.commandWorked(
    testDB.adminCommand({configureFailPoint: 'hangAfterSettingUpIndexBuild', mode: 'off'}));

// Wait for the index build to finish. Since the invalid geo document is removed before the
// index build scans the collection, the index should be built successfully.
createIdx();
IndexBuildTest.assertIndexes(coll, 2, ['_id_', 'b_2dsphere']);

let res = assert.commandWorked(coll.validate({full: true}));
assert(res.valid, 'validation failed on primary: ' + tojson(res));

rst.stopSet();
})();
