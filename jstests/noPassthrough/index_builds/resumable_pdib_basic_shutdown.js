/**
 * Smoke test for the resumable PDIB jstest library.
 *
 * Boots a single-node replica set, starts a primary-driven index build,
 * cleanly shuts down the primary while the build is paused before signalling
 * commit-readiness, restarts, and verifies the build resumed via the
 * "Primary driven" method and ran to completion. commitQuorum must remain
 * kDisabled (0) across the restart.
 *
 * Skips if `featureFlagPrimaryDrivenIndexBuilds` or
 * `featureFlagResumablePrimaryDrivenIndexBuilds` is not enabled — this lets
 * the test sit harmlessly in CI until the resumable-PDIB path lands.
 *
 * See SERVER-125828 / SPM-4469.
 *
 * @tags: [
 *   requires_persistence,
 *   requires_replication,
 * ]
 */

import {runStandalonePdibPauseRestart} from "jstests/libs/index_builds/resumable_pdib_util.js";

const buildUUID = runStandalonePdibPauseRestart({
    indexSpec: {a: 1},
    indexName: "resumable_pdib_basic_shutdown_idx",
    numDocs: 10,
});

if (buildUUID === null) {
    jsTest.log.info("resumable_pdib_basic_shutdown: skipped (resumable-PDIB feature flag off)");
    quit();
}

jsTest.log.info(`resumable_pdib_basic_shutdown: resumed and completed buildUUID=${buildUUID}`);
