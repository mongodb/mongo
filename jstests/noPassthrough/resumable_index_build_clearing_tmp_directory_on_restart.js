/**
 * Tests that a resumable index build clears "_tmp" directory except for files for the build on
 * startup recovery.
 *
 * @tags: [
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   requires_replication,
 * ]
 */
(function() {
"use strict";

load("jstests/noPassthrough/libs/index_build.js");

const dbName = "test";

const numDocuments = 100;
const maxIndexBuildMemoryUsageMB = 50;

const rst = new ReplSetTest({
    nodes: 1,
    nodeOptions: {setParameter: {maxIndexBuildMemoryUsageMegabytes: maxIndexBuildMemoryUsageMB}}
});
rst.startSet();
rst.initiate();

// Insert enough data so that the collection scan spills to disk.
const primary = rst.getPrimary();
const coll = primary.getDB(dbName).getCollection(jsTestName());
const bulk = coll.initializeUnorderedBulkOp();
for (let i = 0; i < numDocuments; i++) {
    // Each document is at least 1 MB.
    bulk.insert({a: i.toString().repeat(1024 * 1024)});
}
assert.commandWorked(bulk.execute());

// Manually writes a garbage file to "_tmp" directory.
const tmpDir = primary.dbpath + "/_tmp/";
mkdir(tmpDir);
writeFile(tmpDir + "garbage", "");

// Runs a resumable index build till completed to make sure the spilled files in "_tmp" directory
// are not deleted with the garbage file.
ResumableIndexBuildTest.run(
    rst,
    dbName,
    coll.getName(),
    [[{a: 1}]],
    [{name: "hangIndexBuildDuringCollectionScanPhaseBeforeInsertion", logIdWithBuildUUID: 20386}],
    // Each document is at least 1 MB, so the index build must have spilled to disk by this point.
    maxIndexBuildMemoryUsageMB,
    ["collection scan"],
    [{numScannedAfterResume: numDocuments - maxIndexBuildMemoryUsageMB}]);

// Asserts the garbage file is deleted.
const files = listFiles(tmpDir);
assert.eq(files.length, 0, files);

rst.stopSet();
})();