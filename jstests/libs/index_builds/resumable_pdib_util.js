/**
 * Reusable helpers for resumable primary-driven index build (PDIB) jstests.
 *
 * Anticipates SERVER-125828 (Resumable PDIB jstest library) under epic SPM-4469.
 * Unlike the legacy `ResumableIndexBuildTest` class in
 * `jstests/noPassthrough/libs/index_builds/index_build.js`, which exercises the
 * secondary-driven resumable code paths (collection scan / bulk load / drain
 * writes phases on a node that owns the build), this module exercises a
 * primary-driven build that is interrupted by *clean shutdown of the primary*
 * and verifies:
 *
 *   - the build resume state was written to disk on shutdown (log 4841502),
 *   - the build is restarted post-startup via the "Primary driven" method
 *     (log 20660),
 *   - the build completes (log 20663),
 *   - commitQuorum stays kDisabled (0) before and after restart — PDIB does
 *     not honour `setIndexCommitQuorum`.
 *
 * PDIB-specific failpoints:
 *   - hangIndexBuildBeforeSignalPrimaryForCommitReadiness: pauses the build on
 *     the primary right before it signals itself as commit-ready, which is the
 *     canonical safe pre-commit pause point for PDIB.
 *   - hangBeforeBuildingIndex: shared pre-collection-scan pause point.
 *
 * All helpers no-op (skip) if `featureFlagPrimaryDrivenIndexBuilds` is not
 * enabled — callers should still gate their fixture setup on the same flag
 * for clarity.
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";

const kPdibFlag = "PrimaryDrivenIndexBuilds";
const kResumableFlag = "ResumablePrimaryDrivenIndexBuilds";

// Canonical PDIB log ids we wait on. Defined as constants so consumers can
// reference them without duplicating magic numbers.
export const kPdibLogIds = Object.freeze({
    // "Index build: writing resume state to disk" — written during shutdown
    // when the resumable path is active.
    resumeStateWritten: 4841502,
    // "Index build: starting" — restart marker with method="Primary driven".
    indexBuildStarting: 20660,
    // "Index build: completed successfully".
    indexBuildCompleted: 20663,
    // "Index build: not restarted from disk" — written when the build is NOT
    // resumable (asserted absent when we expect resume).
    indexBuildNotRestarted: 20347,
});

export const kPdibFailPoints = Object.freeze({
    hangBeforeCommitReadiness: "hangIndexBuildBeforeSignalPrimaryForCommitReadiness",
    hangBeforeBuildingIndex: "hangBeforeBuildingIndex",
});

/**
 * Returns true iff PDIB + resumable-PDIB are both enabled on `conn`. Callers
 * should `quit()` early when this returns false so the suite doesn't waste
 * fixture setup on a no-op run.
 */
export function isResumablePdibEnabled(conn) {
    const db = conn.getDB("admin");
    if (!FeatureFlagUtil.isPresentAndEnabled(db, kPdibFlag)) {
        return false;
    }
    // The resumable-PDIB flag may not yet exist in older binaries; treat
    // missing-but-not-explicitly-disabled as not enabled.
    try {
        return FeatureFlagUtil.isPresentAndEnabled(db, kResumableFlag);
    } catch (e) {
        return false;
    }
}

/**
 * Asserts that PDIB does not honour `setIndexCommitQuorum`. PDIB always runs
 * at commitQuorum=0 (kDisabled); attempts to raise it should fail with
 * BadValue. Use this both before and after restart to pin the invariant.
 */
export function assertCommitQuorumIsDisabled(db, collName, indexNames) {
    assert.commandFailedWithCode(
        db.runCommand({setIndexCommitQuorum: collName, indexNames: indexNames, commitQuorum: 1}),
        ErrorCodes.BadValue,
        "Expected setIndexCommitQuorum to fail with BadValue for PDIB index " + tojson(indexNames),
    );
}

/**
 * Resolves the buildUUID for a single in-progress index build identified by
 * `indexName`. The collection should have exactly one in-progress build at
 * call time.
 */
export function getBuildUUID(coll, indexName) {
    const indexes = IndexBuildTest.assertIndexes(
        coll,
        /* numIndexes */ 2,
        ["_id_"],
        [indexName],
        {includeBuildUUIDs: true},
    );
    return extractUUIDFromObject(indexes[indexName].buildUUID);
}

/**
 * Pause-and-restart driver for a single PDIB. Steps:
 *
 *   1. Enable `pauseFailPoint` on the primary.
 *   2. Start the index build in a parallel shell; expect it to terminate with
 *      InterruptedDueToReplStateChange when the primary is stopped.
 *   3. Wait for the failpoint to be hit, extract the buildUUID.
 *   4. Assert commitQuorum=0 (pre-restart).
 *   5. Clean-shutdown the primary; confirm the resume state was persisted
 *      (log 4841502).
 *   6. Restart the primary with `pauseFailPoint` re-armed via setParameter so
 *      the resumed build pauses again at the same point.
 *   7. Wait for re-election, assert commitQuorum=0 (post-restart), then
 *      release the failpoint.
 *   8. Verify the resumed build was restarted (log 20660 with
 *      method="Primary driven") and ran to completion (log 20663).
 *
 * Returns the buildUUID string for the caller's own assertions.
 *
 * Required: `rst` must be a single-node ReplSetTest (or a set where the
 * caller has already arranged for no failover during shutdown).
 */
export function runPdibPauseRestart({
    rst,
    dbName,
    collName,
    indexSpec,
    indexName = "resumable_pdib_index",
    pauseFailPoint = kPdibFailPoints.hangBeforeCommitReadiness,
}) {
    let primary = rst.getPrimary();
    const primaryDB = primary.getDB(dbName);
    const coll = primaryDB.getCollection(collName);
    const collNss = coll.getFullName();

    jsTest.log.info(`[resumable-pdib] Pausing primary at failpoint: ${pauseFailPoint}`);
    let primFp = configureFailPoint(primary, pauseFailPoint);

    jsTest.log.info("[resumable-pdib] Starting index build on primary");
    const awaitIndexBuild = IndexBuildTest.startIndexBuild(
        primary,
        collNss,
        indexSpec,
        {name: indexName},
        [ErrorCodes.InterruptedDueToReplStateChange],
    );
    primFp.wait();

    const buildUUID = getBuildUUID(coll, indexName);
    jsTest.log.info(`[resumable-pdib] buildUUID=${buildUUID}`);

    assertCommitQuorumIsDisabled(primaryDB, collName, [indexName]);

    jsTest.log.info("[resumable-pdib] Clean-shutting-down primary");
    clearRawMongoProgramOutput();
    rst.stop(primary, undefined, {forRestart: true, skipValidation: true});
    awaitIndexBuild({checkExitSuccess: false});

    // Resume state must have been written to disk during clean shutdown.
    assert(
        RegExp(`${kPdibLogIds.resumeStateWritten}.*${buildUUID}`).test(rawMongoProgramOutput(".*")),
        `Expected resume-state-written log ${kPdibLogIds.resumeStateWritten} for build ${buildUUID}`,
    );
    // "Not restarted" log MUST be absent — that would mean the build was
    // dropped instead of resumed.
    assert.eq(
        false,
        RegExp(`${kPdibLogIds.indexBuildNotRestarted}.*${buildUUID}`).test(rawMongoProgramOutput(".*")),
        `Unexpected not-restarted log ${kPdibLogIds.indexBuildNotRestarted} for build ${buildUUID}`,
    );

    jsTest.log.info("[resumable-pdib] Restarting primary with failpoint re-armed");
    rst.start(
        primary,
        {setParameter: `failpoint.${pauseFailPoint}={mode:'alwaysOn'}`},
        /* restart */ true,
    );

    primary = rst.getPrimary();
    const restartedDB = primary.getDB(dbName);
    assertCommitQuorumIsDisabled(restartedDB, collName, [indexName]);

    jsTest.log.info("[resumable-pdib] Releasing failpoint, awaiting completion");
    assert.commandWorked(primary.adminCommand({configureFailPoint: pauseFailPoint, mode: "off"}));

    // Restart marker: method="Primary driven".
    checkLog.containsJson(primary, kPdibLogIds.indexBuildStarting, {
        buildUUID: function (uuid) {
            return uuid && uuid["uuid"]["$uuid"] === buildUUID;
        },
        method: "Primary driven",
    });

    // Final completion.
    checkLog.containsJson(primary, kPdibLogIds.indexBuildCompleted, {
        buildUUID: function (uuid) {
            return uuid && uuid["uuid"]["$uuid"] === buildUUID;
        },
        namespace: restartedDB.getCollection(collName).getFullName(),
    });

    // The index should now exist on the primary post-restart.
    IndexBuildTest.assertIndexes(restartedDB.getCollection(collName), 2, ["_id_", indexName]);

    return buildUUID;
}

/**
 * Convenience wrapper that boots a single-node replica set, seeds `numDocs`,
 * runs `runPdibPauseRestart`, and tears down. Returns the buildUUID.
 *
 * Returns null if resumable PDIB is not enabled on the binary under test.
 */
export function runStandalonePdibPauseRestart({
    dbName = jsTestName(),
    collName = "coll",
    indexSpec = {a: 1},
    indexName = "resumable_pdib_index",
    numDocs = 10,
    pauseFailPoint = kPdibFailPoints.hangBeforeCommitReadiness,
} = {}) {
    const rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate(null, null, {initiateWithDefaultElectionTimeout: true});

    if (!isResumablePdibEnabled(rst.getPrimary())) {
        jsTest.log.info("[resumable-pdib] Skipping — resumable-PDIB is not enabled on this binary");
        rst.stopSet();
        return null;
    }

    const primary = rst.getPrimary();
    const coll = primary.getDB(dbName).getCollection(collName);
    assert.commandWorked(primary.getDB(dbName).runCommand({create: collName}));
    const bulk = coll.initializeUnorderedBulkOp();
    for (let i = 0; i < numDocs; i++) bulk.insert({a: i});
    assert.commandWorked(bulk.execute());
    rst.awaitReplication();

    const buildUUID = runPdibPauseRestart({
        rst,
        dbName,
        collName,
        indexSpec,
        indexName,
        pauseFailPoint,
    });

    rst.stopSet();
    return buildUUID;
}
