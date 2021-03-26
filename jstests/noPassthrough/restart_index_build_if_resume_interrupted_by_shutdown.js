/**
 * Tests that an index build is resumable only once across restarts. If the resumed index build
 * fails to run to completion before shutdown, it will restart from the beginning on the next server
 * startup.
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
const collName = jsTestName();

let rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

let primary = rst.getPrimary();

if (!ResumableIndexBuildTest.resumableIndexBuildsEnabled(primary)) {
    jsTestLog("Skipping test because resumable index builds are not enabled");
    rst.stopSet();
    return;
}

ResumableIndexBuildTest.runResumeInterruptedByShutdown(
    rst,
    dbName,
    collName + "_collscan_drain",
    {a: 1},                    // index key pattern
    "resumable_index_build1",  // index name
    {name: "hangIndexBuildDuringCollectionScanPhaseBeforeInsertion", logIdWithBuildUUID: 20386},
    "collection scan",
    {a: 1},  // initial doc
    [{a: 2}, {a: 3}],
    [{a: 4}, {a: 5}]);

ResumableIndexBuildTest.runResumeInterruptedByShutdown(
    rst,
    dbName,
    collName + "_bulkload_drain_multikey",
    {a: 1},                    // index key pattern
    "resumable_index_build2",  // index name
    {name: "hangIndexBuildDuringBulkLoadPhase", logIdWithIndexName: 4924400},
    "bulk load",
    {a: [11, 22, 33]},  // initial doc
    [{a: 77}, {a: 88}],
    [{a: 99}, {a: 100}]);

rst.stopSet();
})();
