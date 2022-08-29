/**
 * Tests that serverStatus contains an indexBuilder section. This section reports
 * globally-aggregated statistics about index builds and the external sorter.
 *
 * @tags: [
 *   requires_persistence,
 *   requires_replication,
 * ]
 */
(function() {
'use strict';

load('jstests/noPassthrough/libs/index_build.js');

const replSet = new ReplSetTest({
    nodes: 1,
    nodeOptions: {setParameter: {maxIndexBuildMemoryUsageMegabytes: 50}},
});
replSet.startSet();
replSet.initiate();

let primary = replSet.getPrimary();
let testDB = primary.getDB('test');
let coll = testDB.getCollection('t');

for (let i = 0; i < 10; i++) {
    assert.commandWorked(coll.insert({
        _id: i,
        a: 'a'.repeat(10 * 1024 * 1024),
    }));
}

assert.commandWorked(coll.createIndex({a: 1}));

let serverStatus = testDB.serverStatus();
assert(serverStatus.hasOwnProperty('indexBulkBuilder'),
       'indexBuildBuilder section missing: ' + tojson(serverStatus));

let indexBulkBuilderSection = serverStatus.indexBulkBuilder;
assert.eq(indexBulkBuilderSection.count, 1, tojson(indexBulkBuilderSection));
assert.eq(indexBulkBuilderSection.filesOpenedForExternalSort, 4, tojson(indexBulkBuilderSection));
assert.eq(indexBulkBuilderSection.filesClosedForExternalSort, 4, tojson(indexBulkBuilderSection));

// Confirm that metrics are updated during initial sync.
const newNode = replSet.add({setParameter: {maxIndexBuildMemoryUsageMegabytes: 50}});
replSet.reInitiate();
replSet.waitForState(newNode, ReplSetTest.State.SECONDARY);
replSet.awaitReplication();
let newNodeTestDB = newNode.getDB(testDB.getName());
let newNodeColl = newNodeTestDB.getCollection(coll.getName());
IndexBuildTest.assertIndexes(newNodeColl, 2, ['_id_', 'a_1']);
indexBulkBuilderSection = newNodeTestDB.serverStatus().indexBulkBuilder;
jsTestLog('initial sync server status: ' + tojson(indexBulkBuilderSection));
// We expect initial sync to build at least three indexes for the test collection in addition
// to indexes for internal collections required for the proper running of the server.
// The test collection has the only index that will cause the external sorter to spill to disk,
// so the file descriptor open/closed counters should each report a value comparable to that for
// a single index build that spills to disk.
// Also, 4.2 does not contain the external sorter improvements in SERVER-54761, so the numbers
// reported in the server status for file handle activity will be a little different from those
// for similar index builds in 4.4.
assert.gte(indexBulkBuilderSection.count, 1, tojson(indexBulkBuilderSection));
assert.gte(indexBulkBuilderSection.filesOpenedForExternalSort, 4, tojson(indexBulkBuilderSection));
assert.gte(indexBulkBuilderSection.filesClosedForExternalSort, 4, tojson(indexBulkBuilderSection));

// Building multiple indexes in a single createIndex command increases count by the number of
// indexes requested.
// The compound index is the only index that will cause the sorter to use the disk because it
// indexes large values in the field 'a'.
// The numbers here will be different from those reported for 4.4 because 4.4 contains the external
// sorter improvements in SERVER-54761.
assert.commandWorked(coll.createIndexes([{c: 1}, {d: 1}, {e: 1, a: 1}]));
IndexBuildTest.assertIndexes(newNodeColl, 5, ['_id_', 'a_1', 'c_1', 'd_1', 'e_1_a_1']);
indexBulkBuilderSection = testDB.serverStatus().indexBulkBuilder;
jsTestLog('server status after building multiple indexes: ' + tojson(indexBulkBuilderSection));
assert.eq(indexBulkBuilderSection.count, 4, tojson(indexBulkBuilderSection));
assert.gte(indexBulkBuilderSection.filesOpenedForExternalSort, 8, tojson(indexBulkBuilderSection));
assert.gte(indexBulkBuilderSection.filesClosedForExternalSort, 8, tojson(indexBulkBuilderSection));

replSet.stopSet();
})();
