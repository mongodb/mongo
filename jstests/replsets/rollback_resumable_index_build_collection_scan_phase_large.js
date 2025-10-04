/**
 * Tests that a resumable index build large enough to spill to disk during the collection scan
 * phase completes properly after being interrupted for rollback during the collection scan phase.
 *
 * @tags: [
 *   # Primary-driven index builds aren't resumable.
 *   primary_driven_index_builds_incompatible,
 *   # The rollback can be slow on certain build variants (such as macOS and code coverage), which
 *   # can cause the targeted log messages to fall off the log buffer before we search for them.
 *   incompatible_with_macos,
 *   incompatible_with_gcov,
 *   requires_majority_read_concern,
 *   requires_persistence,
 * ]
 */
import {RollbackResumableIndexBuildTest} from "jstests/replsets/libs/rollback_resumable_index_build.js";
import {RollbackTest} from "jstests/replsets/libs/rollback_test.js";

const dbName = "test";

const numDocuments = 200;
const maxIndexBuildMemoryUsageMB = 50;

const rollbackTest = new RollbackTest(jsTestName());

// Insert enough data so that the collection scan spills to disk. Keep the size of each document
// small enough to report for validation, in case there are index inconsistencies.
const docs = [];
for (let i = 0; i < numDocuments; i++) {
    // Since most integers will take two or three bytes, almost all documents are at least 0.5 MB.
    docs.push({a: i.toString().repeat(1024 * 256)});
}

const runRollbackTo = function (rollbackEndFailPointName, rollbackEndFailPointLogIdWithBuildUUID) {
    assert.commandWorked(
        rollbackTest
            .getPrimary()
            .adminCommand({setParameter: 1, maxIndexBuildMemoryUsageMegabytes: maxIndexBuildMemoryUsageMB}),
    );

    RollbackResumableIndexBuildTest.run(
        rollbackTest,
        dbName,
        "_large",
        docs,
        [[{a: 1}]],
        [
            {
                name: "hangIndexBuildDuringCollectionScanPhaseBeforeInsertion",
                logIdWithBuildUUID: 20386,
            },
        ],
        // Most documents are at least 0.5 MB, so the index build must have spilled to disk by this
        // point.
        maxIndexBuildMemoryUsageMB, // rollbackStartFailPointsIteration
        [
            {
                name: rollbackEndFailPointName,
                logIdWithBuildUUID: rollbackEndFailPointLogIdWithBuildUUID,
            },
        ],
        1, // rollbackEndFailPointsIteration
        ["setYieldAllLocksHang"],
        ["collection scan"],
        // The collection scan will scan one additional document past the point specified above due
        // to locks needing to be yielded before the rollback can occur. Thus, we subtract 1 from
        // the difference.
        [{numScannedAfterResume: numDocuments - maxIndexBuildMemoryUsageMB - 1}],
        [{a: 1}, {a: 2}],
    );
};

// Rollback to before the index begins to be built.
runRollbackTo("hangAfterSettingUpIndexBuild", 20387);

// Rollback to earlier in the collection scan phase.
runRollbackTo("hangIndexBuildDuringCollectionScanPhaseAfterInsertion", 20386);

rollbackTest.stop();
