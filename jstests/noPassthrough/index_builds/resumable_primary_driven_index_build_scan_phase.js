/**
 * Tests resuming a primary-driven index build in the scan phase.
 *
 * @tags: [
 *   requires_otel_build,
 *   requires_persistence,
 *   requires_replication,
 * ]
 */

import {
    PdibPhase,
    PrimaryDrivenResumableIndexBuildTest,
} from "jstests/noPassthrough/libs/index_builds/primary_driven.js";

const rst = PrimaryDrivenResumableIndexBuildTest.setUp();
PrimaryDrivenResumableIndexBuildTest.run(rst, {phase: PdibPhase.SCAN});
PrimaryDrivenResumableIndexBuildTest.tearDown(rst);
