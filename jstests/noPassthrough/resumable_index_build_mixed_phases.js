/**
 * Tests that resumable index builds in different phases write their state to disk upon clean
 * shutdown and are resumed from the same phase to completion when the node is started back up.
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
load("jstests/libs/sbe_util.js");          // For checkSBEEnabled.
load("jstests/libs/columnstore_util.js");  // For setUpServerForColumnStoreIndexTest.

const dbName = "test";

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const columnstoreEnabled =
    checkSBEEnabled(rst.getPrimary().getDB(dbName), ["featureFlagColumnstoreIndexes"], true) &&
    setUpServerForColumnStoreIndexTest(rst.getPrimary().getDB(dbName));

const runTest = function(docs, indexSpecs, failPoints, resumePhases, resumeChecks, collNameSuffix) {
    const coll = rst.getPrimary().getDB(dbName).getCollection(
        jsTestName() + "_" + resumePhases[0].replace(" ", "_") + "_" +
        resumePhases[1].replace(" ", "_") + collNameSuffix);
    assert.commandWorked(coll.insert(docs));

    ResumableIndexBuildTest.run(rst,
                                dbName,
                                coll.getName(),
                                indexSpecs,
                                failPoints,
                                1,
                                resumePhases,
                                resumeChecks,
                                [{a: 4, b: 4}, {a: 5, b: 5}, {a: 6, b: 6}],
                                [{a: 7, b: 7}, {a: 8, b: 8}, {a: 9, b: 9}]);
};

const runTests = function(failPoints, resumePhases, resumeChecks) {
    runTest([{a: 1, b: 1}, {a: 2, b: 2}, {a: 3, b: 3}],
            [[{a: 1}], [{b: 1}]],
            failPoints,
            resumePhases,
            resumeChecks,
            "");
    runTest([{a: [1, 2]}, {a: 2, b: 2}, {a: 3, b: [3, 4]}],
            [[{a: 1}], [{b: 1}]],
            failPoints,
            resumePhases,
            resumeChecks,
            "_multikey");
    runTest([{a: [1, 2], b: {c: [3, 4]}}, {d: "", e: "", f: [[]]}, {g: null, h: 8}],
            [[{"$**": 1}], [{h: 1}]],
            failPoints,
            resumePhases,
            resumeChecks,
            "_wildcard");

    if (columnstoreEnabled) {
        runTest([{a: 1, b: 1}, {a: 2, b: 2}, {a: 3, b: 3}],
                [[{"$**": "columnstore"}], [{b: 1}]],
                failPoints,
                resumePhases,
                resumeChecks,
                "_columnstore");
    }
};

runTests(
    [
        {name: "hangIndexBuildBeforeWaitingUntilMajorityOpTime", logIdWithBuildUUID: 4940901},
        {name: "hangIndexBuildDuringCollectionScanPhaseBeforeInsertion", logIdWithBuildUUID: 20386}
    ],
    ["initialized", "collection scan"],
    [{numScannedAfterResume: 6}, {numScannedAfterResume: 5}]);
runTests(
    [
        {name: "hangIndexBuildBeforeWaitingUntilMajorityOpTime", logIdWithBuildUUID: 4940901},
        {name: "hangIndexBuildDuringBulkLoadPhase", logIdWithIndexName: 4924400}
    ],
    ["initialized", "bulk load"],
    [{numScannedAfterResume: 6}, {skippedPhaseLogID: 20391}]);
runTests(
    [
        {name: "hangIndexBuildBeforeWaitingUntilMajorityOpTime", logIdWithBuildUUID: 4940901},
        {name: "hangIndexBuildDuringDrainWritesPhase", logIdWithIndexName: 4841800}
    ],
    ["initialized", "drain writes"],
    [{numScannedAfterResume: 6}, {skippedPhaseLogID: 20392}]);
runTests(
    [
        {name: "hangIndexBuildDuringCollectionScanPhaseBeforeInsertion", logIdWithBuildUUID: 20386},
        {name: "hangIndexBuildDuringBulkLoadPhase", logIdWithIndexName: 4924400}
    ],
    ["collection scan", "bulk load"],
    [{numScannedAfterResume: 5}, {skippedPhaseLogID: 20391}]);
runTests(
    [
        {name: "hangIndexBuildDuringCollectionScanPhaseBeforeInsertion", logIdWithBuildUUID: 20386},
        {name: "hangIndexBuildDuringDrainWritesPhase", logIdWithIndexName: 4841800}
    ],
    ["collection scan", "drain writes"],
    [{numScannedAfterResume: 5}, {skippedPhaseLogID: 20392}]);
runTests(
    [
        {name: "hangIndexBuildDuringBulkLoadPhase", logIdWithIndexName: 4924400},
        {name: "hangIndexBuildDuringDrainWritesPhase", logIdWithIndexName: 4841800}
    ],
    ["bulk load", "drain writes"],
    [{skippedPhaseLogID: 20391}, {skippedPhaseLogID: 20392}]);
rst.stopSet();
})();
