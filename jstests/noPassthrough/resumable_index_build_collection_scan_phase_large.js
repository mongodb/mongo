/**
 * Tests that a resumable index build large enough to spill to disk during the collection scan
 * phase writes its state to disk upon clean shutdown during the collection scan phase and is
 * resumed from the same phase to completion when the node is started back up.
 *
 * @tags: [
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   requires_replication,
 *   sbe_incompatible,
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
const coll = rst.getPrimary().getDB(dbName).getCollection(jsTestName());
const bulk = coll.initializeUnorderedBulkOp();
for (let i = 0; i < numDocuments; i++) {
    // Each document is at least 1 MB.
    bulk.insert({a: i.toString().repeat(1024 * 1024)});
}
assert.commandWorked(bulk.execute());

ResumableIndexBuildTest.run(
    rst,
    dbName,
    coll.getName(),
    [[{a: 1}]],
    [{name: "hangIndexBuildDuringCollectionScanPhaseBeforeInsertion", logIdWithBuildUUID: 20386}],
    // Each document is at least 1 MB, so the index build must have spilled to disk by this point.
    maxIndexBuildMemoryUsageMB,
    ["collection scan"],
    [{numScannedAferResume: numDocuments - maxIndexBuildMemoryUsageMB}]);

rst.stopSet();
})();