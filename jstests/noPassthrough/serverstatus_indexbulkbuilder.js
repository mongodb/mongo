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
});
replSet.startSet();
replSet.initiate();

let primary = replSet.getPrimary();
let testDB = primary.getDB('test');
let coll = testDB.getCollection('t');

for (let i = 0; i < 10; i++) {
    assert.commandWorked(coll.insert({
        _id: i,
        a: i,
    }));
}

assert.commandWorked(coll.createIndex({a: 1}));

let serverStatus = testDB.serverStatus();
assert(serverStatus.hasOwnProperty('indexBulkBuilder'),
       'indexBuildBuilder section missing: ' + tojson(serverStatus));

let indexBulkBuilderSection = serverStatus.indexBulkBuilder;
assert.eq(indexBulkBuilderSection.count, 1, tojson(indexBulkBuilderSection));
assert.eq(indexBulkBuilderSection.resumed, 0, tojson(indexBulkBuilderSection));

// Shut down server during an index to verify 'resumable' value on restart.
IndexBuildTest.pauseIndexBuilds(primary);
const createIdx = IndexBuildTest.startIndexBuild(primary,
                                                 coll.getFullName(),
                                                 {b: 1},
                                                 /*options=*/{},
                                                 [ErrorCodes.InterruptedDueToReplStateChange]);
IndexBuildTest.waitForIndexBuildToScanCollection(testDB, coll.getName(), 'b_1');
const buildUUID = extractUUIDFromObject(
    IndexBuildTest
        .assertIndexes(coll, 3, ['_id_', 'a_1'], ['b_1'], {includeBuildUUIDs: true})['b_1']
        .buildUUID);
replSet.restart(/*nodeId=*/0);
createIdx();
primary = replSet.getPrimary();
testDB = primary.getDB('test');
coll = testDB.getCollection('t');
ResumableIndexBuildTest.assertCompleted(primary, coll, [buildUUID], ['a_1', 'b_1']);
indexBulkBuilderSection = testDB.serverStatus().indexBulkBuilder;
assert.eq(indexBulkBuilderSection.count, 1, tojson(indexBulkBuilderSection));
assert.eq(indexBulkBuilderSection.resumed, 1, tojson(indexBulkBuilderSection));

replSet.stopSet();
})();
