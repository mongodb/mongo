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

const maxMemUsageMegabytes = 50;
const numDocs = 10;
const fieldSize = 10 * 1024 * 1024;
const approxMemoryUsage = numDocs * fieldSize;
let expectedSpilledRanges = approxMemoryUsage / (maxMemUsageMegabytes * 1024 * 1024);

const replSet = new ReplSetTest({
    nodes: 1,
    nodeOptions: {setParameter: {maxIndexBuildMemoryUsageMegabytes: maxMemUsageMegabytes}},
});
replSet.startSet();
replSet.initiate();

let primary = replSet.getPrimary();
let testDB = primary.getDB('test');
let coll = testDB.getCollection('t');

for (let i = 0; i < numDocs; i++) {
    assert.commandWorked(coll.insert({
        _id: i,
        a: 'a'.repeat(fieldSize),
    }));
}

assert.commandWorked(coll.createIndex({a: 1}));

let serverStatus = testDB.serverStatus();
assert(serverStatus.hasOwnProperty('indexBulkBuilder'),
       'indexBuildBuilder section missing: ' + tojson(serverStatus));

let indexBulkBuilderSection = serverStatus.indexBulkBuilder;
assert.eq(indexBulkBuilderSection.count, 1, tojson(indexBulkBuilderSection));
assert.eq(indexBulkBuilderSection.resumed, 0, tojson(indexBulkBuilderSection));
assert.eq(indexBulkBuilderSection.filesOpenedForExternalSort, 1, tojson(indexBulkBuilderSection));
assert.eq(indexBulkBuilderSection.filesClosedForExternalSort, 1, tojson(indexBulkBuilderSection));
assert.eq(
    indexBulkBuilderSection.spilledRanges, expectedSpilledRanges, tojson(indexBulkBuilderSection));
assert.between(0,
               indexBulkBuilderSection.bytesSpilled,
               approxMemoryUsage,
               tojson(indexBulkBuilderSection),
               true);

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
// Even though the amount of index data for b_1 is well under the configured memory usage limit,
// the resumable index build logic dictates that we spill the sorter data to disk on shutdown
// and read it back on startup.
assert.eq(indexBulkBuilderSection.filesOpenedForExternalSort, 1, tojson(indexBulkBuilderSection));
assert.eq(indexBulkBuilderSection.filesClosedForExternalSort, 1, tojson(indexBulkBuilderSection));
assert.eq(indexBulkBuilderSection.spilledRanges, 1, tojson(indexBulkBuilderSection));
assert.between(0,
               indexBulkBuilderSection.bytesSpilled,
               approxMemoryUsage,
               tojson(indexBulkBuilderSection),
               true);

// Confirm that metrics are updated during initial sync.
const newNode =
    replSet.add({setParameter: {maxIndexBuildMemoryUsageMegabytes: maxMemUsageMegabytes}});
replSet.reInitiate();
replSet.waitForState(newNode, ReplSetTest.State.SECONDARY);
replSet.awaitReplication();
let newNodeTestDB = newNode.getDB(testDB.getName());
let newNodeColl = newNodeTestDB.getCollection(coll.getName());
IndexBuildTest.assertIndexes(newNodeColl, 3, ['_id_', 'a_1', 'b_1']);
indexBulkBuilderSection = newNodeTestDB.serverStatus().indexBulkBuilder;
// We expect initial sync to build at least three indexes for the test collection in addition
// to indexes for internal collections required for the proper running of the server.
// The test collection has the only index that will cause the external sorter to spill to disk,
// so the file descriptor open/closed counters should each report a value of one.
assert.gte(indexBulkBuilderSection.count, 3, tojson(indexBulkBuilderSection));
assert.eq(indexBulkBuilderSection.filesOpenedForExternalSort, 1, tojson(indexBulkBuilderSection));
assert.eq(indexBulkBuilderSection.filesClosedForExternalSort, 1, tojson(indexBulkBuilderSection));
// We try building two indexes for the test collection so the memory usage limit for each index
// build during initial sync is the maxIndexBuildMemoryUsageMegabytes divided by the number of index
// builds. We end up with half of the in-memory memory so we double the amount of spills expected.
expectedSpilledRanges *= 2;
assert.eq(
    indexBulkBuilderSection.spilledRanges, expectedSpilledRanges, tojson(indexBulkBuilderSection));
assert.between(0,
               indexBulkBuilderSection.bytesSpilled,
               approxMemoryUsage,
               tojson(indexBulkBuilderSection),
               true);

// Building multiple indexes in a single createIndex command increases count by the number of
// indexes requested.
// The compound index is the only index that will cause the sorter to use the disk because it
// indexes large values in the field 'a'.
// The expected values in the server status should add to the numbers at the end of the resumable
// index build test case.
assert.commandWorked(coll.createIndexes([{c: 1}, {d: 1}, {e: 1, a: 1}]));
IndexBuildTest.assertIndexes(coll, 6, ['_id_', 'a_1', 'b_1', 'c_1', 'd_1', 'e_1_a_1']);
indexBulkBuilderSection = testDB.serverStatus().indexBulkBuilder;
assert.eq(indexBulkBuilderSection.count, 4, tojson(indexBulkBuilderSection));
assert.eq(indexBulkBuilderSection.resumed, 1, tojson(indexBulkBuilderSection));
assert.eq(indexBulkBuilderSection.filesOpenedForExternalSort, 2, tojson(indexBulkBuilderSection));
assert.eq(indexBulkBuilderSection.filesClosedForExternalSort, 2, tojson(indexBulkBuilderSection));
expectedSpilledRanges += 2;
assert.eq(
    indexBulkBuilderSection.spilledRanges, expectedSpilledRanges, tojson(indexBulkBuilderSection));
assert.between(0,
               indexBulkBuilderSection.bytesSpilled,
               approxMemoryUsage,
               tojson(indexBulkBuilderSection),
               true);

replSet.stopSet();
})();
