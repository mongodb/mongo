/**
 * Tests that serverStatus contains an indexBuilder section. This section reports
 * globally-aggregated statistics about index builds and the external sorter.
 *
 * @tags: [
 *   requires_persistence,
 *   requires_replication,
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";
import {IndexBuildTest, ResumableIndexBuildTest} from "jstests/noPassthrough/libs/index_build.js";

const maxMemUsageMegabytes = 50;
// 10% (maxIteratorsMemoryUsagePercentage) and up to 1MB of maxMemUsageMegabytes is used to store
// the file iterators used during external sorting.
const maxDataMemUsageMegabytes = maxMemUsageMegabytes - Math.min(maxMemUsageMegabytes * 0.1, 1);
const numDocs = 10;
const fieldSize = 10 * 1024 * 1024;
const approxMemoryUsage = numDocs * fieldSize;
// memPool allocates memory that is the first power of two greater than what is needed. It decides
// whether to spill using this amount of memory.
const memPoolMemoryUsage = (1 << 32 - Math.clz32(fieldSize));
// Due to some overhead in the sorter's memory usage while spilling, add a small padding.
const spillOverhead = numDocs * 16;

// Number of memory allocations that will cause a spill. Due to fragmentation in the memory pool one
// allocation corresponds to one document.
let memAllocNum = Math.trunc((maxDataMemUsageMegabytes * 1024 * 1024 + memPoolMemoryUsage - 1) /
                             memPoolMemoryUsage);
let expectedSpilledRanges = Math.trunc((numDocs + memAllocNum - 1) / memAllocNum);

const documentBsonSize = Object.bsonsize({
    _id: 0,
    a: 'a'.repeat(fieldSize),
});

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

jsTestLog("Creating index 'a'");
assert.commandWorked(coll.createIndex({a: 1}));

let serverStatus = testDB.serverStatus();
assert(serverStatus.hasOwnProperty('indexBulkBuilder'),
       'indexBuildBuilder section missing: ' + tojson(serverStatus));

let indexBulkBuilderSection = serverStatus.indexBulkBuilder;
assert.eq(indexBulkBuilderSection.count, 1, tojson(indexBulkBuilderSection));
assert.eq(indexBulkBuilderSection.resumed, 0, tojson(indexBulkBuilderSection));
assert.eq(indexBulkBuilderSection.filesOpenedForExternalSort, 1, tojson(indexBulkBuilderSection));
assert.eq(indexBulkBuilderSection.filesClosedForExternalSort, 1, tojson(indexBulkBuilderSection));
// Due to fragmentation in the allocator, which is counted towards mem usage, we can spill earlier
// than we would expect.
assert.between(expectedSpilledRanges,
               indexBulkBuilderSection.spilledRanges,
               1 + expectedSpilledRanges,
               tojson(indexBulkBuilderSection),
               /*inclusive=*/ true);
assert.between(0,
               indexBulkBuilderSection.bytesSpilled,
               approxMemoryUsage,
               tojson(indexBulkBuilderSection),
               /*inclusive=*/ true);
// We write out a single byte for every spilled doc, which is included in the uncompressed size.
assert.between(0,
               indexBulkBuilderSection.bytesSpilledUncompressed,
               approxMemoryUsage + spillOverhead,
               tojson(indexBulkBuilderSection),
               /*inclusive=*/ true);
assert.eq(indexBulkBuilderSection.numSorted, numDocs, tojson(indexBulkBuilderSection));
// Expect total bytes sorted to be greater than approxMemoryUsage because of the additional field in
// the documents inserted which accounts for more bytes than in the rough calculation.
assert.gte(indexBulkBuilderSection.bytesSorted, approxMemoryUsage, tojson(indexBulkBuilderSection));
assert.between(0,
               indexBulkBuilderSection.memUsage,
               approxMemoryUsage,
               tojson(indexBulkBuilderSection),
               /*inclusive=*/ true);

(function resumeIndexBuild() {
    jsTestLog(
        "Shutting down server during index build for 'b' to verify 'resumable' value on restart.");
    IndexBuildTest.pauseIndexBuilds(primary);
    const createIdx = IndexBuildTest.startIndexBuild(primary,
                                                     coll.getFullName(),
                                                     {b: 1},
                                                     /*options=*/ {},
                                                     [ErrorCodes.InterruptedDueToReplStateChange]);
    IndexBuildTest.waitForIndexBuildToScanCollection(testDB, coll.getName(), 'b_1');
    const buildUUID = extractUUIDFromObject(
        IndexBuildTest
            .assertIndexes(coll, 3, ['_id_', 'a_1'], ['b_1'], {includeBuildUUIDs: true})['b_1']
            .buildUUID);
    replSet.restart(/*nodeId=*/ 0);
    createIdx();
    primary = replSet.getPrimary();
    testDB = primary.getDB('test');
    coll = testDB.getCollection('t');
    ResumableIndexBuildTest.assertCompleted(primary, coll, [buildUUID], ['a_1', 'b_1']);
})();
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
               /*inclusive=*/ true);
assert.between(0,
               indexBulkBuilderSection.bytesSpilledUncompressed,
               // We write out some extra data for every spilled doc, which is included in the
               // uncompressed size.
               approxMemoryUsage + spillOverhead,
               tojson(indexBulkBuilderSection),
               /*inclusive=*/ true);
assert.eq(indexBulkBuilderSection.numSorted, 0, tojson(indexBulkBuilderSection));
assert.eq(indexBulkBuilderSection.bytesSorted, 0, tojson(indexBulkBuilderSection));
assert.eq(indexBulkBuilderSection.memUsage, 0, tojson(indexBulkBuilderSection));

(function initialSyncNewNode() {
    jsTestLog("Adding new node and initial syncing");
    const newNode =
        replSet.add({setParameter: {maxIndexBuildMemoryUsageMegabytes: maxMemUsageMegabytes}});
    replSet.reInitiate();
    replSet.awaitSecondaryNodes(null, [newNode]);
    replSet.awaitReplication();
    let newNodeTestDB = newNode.getDB(testDB.getName());
    let newNodeColl = newNodeTestDB.getCollection(coll.getName());
    IndexBuildTest.assertIndexes(newNodeColl, 3, ['_id_', 'a_1', 'b_1']);
    assert.eq(newNodeColl.find().hint({a: 1}).count(), 10);
    indexBulkBuilderSection = newNodeTestDB.serverStatus().indexBulkBuilder;
})();
// We expect initial sync to build at least three indexes for the test collection in addition
// to indexes for internal collections required for the proper running of the server.
// The test collection has the only index that will cause the external sorter to spill to disk,
// so the file descriptor open/closed counters should each report a value of one.
assert.gte(indexBulkBuilderSection.count, 3, tojson(indexBulkBuilderSection));
assert.eq(indexBulkBuilderSection.filesOpenedForExternalSort, 1, tojson(indexBulkBuilderSection));
assert.eq(indexBulkBuilderSection.filesClosedForExternalSort, 1, tojson(indexBulkBuilderSection));
// We try building two indexes for the test collection so the memory usage limit for each index
// build during initial sync is the maxIndexBuildMemoryUsageMegabytes divided by the number of index
// builds. We end up with half of the in-memory memory so we double the amount of the memory
// reserved for the file iterators in sort.
let maxMemUsagePerIndexBytes = maxMemUsageMegabytes * 1024 * 1024 / 2;
let maxDataMemUsagePerIndexBytes = maxMemUsagePerIndexBytes - 1024 * 1024;
memAllocNum =
    Math.trunc((maxDataMemUsagePerIndexBytes + memPoolMemoryUsage - 1) / memPoolMemoryUsage);
expectedSpilledRanges = Math.trunc((numDocs + memAllocNum - 1) / memAllocNum);

// Due to fragmentation in the allocator, which is counted towards mem usage, we can spill
// earlier than we would expect.
assert.between(expectedSpilledRanges,
               indexBulkBuilderSection.spilledRanges,
               1 + expectedSpilledRanges,
               tojson(indexBulkBuilderSection),
               /*inclusive=*/ true);
assert.between(0,
               indexBulkBuilderSection.bytesSpilled,
               approxMemoryUsage,
               tojson(indexBulkBuilderSection),
               /*inclusive=*/ true);
assert.between(0,
               indexBulkBuilderSection.bytesSpilledUncompressed,
               // We write out some extra data for every spilled doc, which is included in the
               // uncompressed size.
               approxMemoryUsage + spillOverhead,
               tojson(indexBulkBuilderSection),
               /*inclusive=*/ true);
// Expect at least all documents in the collection are sorted for three indexes but there may also
// be internal indexes with additional sorts.
assert.gte(indexBulkBuilderSection.numSorted, numDocs * 3, tojson(indexBulkBuilderSection));
// We are sorting on index keys and keystring sizes are encoded to its necessary bytes so we are
// approximating that _id and b will be at least 3 bytes each to account for the integer value, the
// end byte, and the record id.
let dataUsage = approxMemoryUsage + (3 * numDocs) + (3 * numDocs);
assert.gte(indexBulkBuilderSection.bytesSorted, dataUsage, tojson(indexBulkBuilderSection));
assert.between(0,
               indexBulkBuilderSection.memUsage,
               approxMemoryUsage,
               tojson(indexBulkBuilderSection),
               /*inclusive=*/ true);

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
expectedSpilledRanges += 1;

// The memory is shared among 3 indexes but only the compound index will spill.
maxMemUsagePerIndexBytes = maxMemUsageMegabytes * 1024 * 1024 / 3;
maxDataMemUsagePerIndexBytes =
    maxMemUsagePerIndexBytes - 1024 * 1024;  // There is enough memory to reserve 1MB for the file
// iterators needed for each index.

memAllocNum =
    Math.trunc((maxDataMemUsagePerIndexBytes + memPoolMemoryUsage - 1) / memPoolMemoryUsage);
expectedSpilledRanges = Math.trunc((numDocs + memAllocNum - 1) / memAllocNum);

assert.between(expectedSpilledRanges,
               indexBulkBuilderSection.spilledRanges,
               1 + expectedSpilledRanges,
               tojson(indexBulkBuilderSection),
               /*inclusive=*/ true);
assert.between(0,
               indexBulkBuilderSection.bytesSpilled,
               approxMemoryUsage,
               tojson(indexBulkBuilderSection),
               /*inclusive=*/ true);
assert.between(0,
               indexBulkBuilderSection.bytesSpilledUncompressed,
               // We write out some extra data for every spilled doc, which is included in the
               // uncompressed size.
               approxMemoryUsage + spillOverhead,
               tojson(indexBulkBuilderSection),
               /*inclusive=*/ true);
assert.eq(indexBulkBuilderSection.numSorted, numDocs * 3, tojson(indexBulkBuilderSection));
// We are sorting on index keys and keystring sizes are encoded to its necessary bytes so we are
// approximating that _id and b will be at least 3 bytes each to account for the integer value, the
// end byte, and the record id.
dataUsage = approxMemoryUsage + (3 * numDocs) + (3 * numDocs);
assert.gte(indexBulkBuilderSection.bytesSorted, dataUsage, tojson(indexBulkBuilderSection));
assert.between(0,
               indexBulkBuilderSection.memUsage,
               approxMemoryUsage,
               tojson(indexBulkBuilderSection),
               /*inclusive=*/ true);

replSet.stopSet();
