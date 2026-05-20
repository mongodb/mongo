/**
 * Tests resuming a single primary-driven index build multiple times in a row -- once in the middle
 * of each phase (scan, load, drain).
 *
 * @tags: [
 *   requires_otel_build,
 *   requires_persistence,
 *   requires_replication,
 * ]
 */

import {
    PdibPhase,
    PdibPosition,
    PrimaryDrivenResumableIndexBuildTest,
} from "jstests/noPassthrough/libs/index_builds/primary_driven.js";

const rst = PrimaryDrivenResumableIndexBuildTest.setUp();
PrimaryDrivenResumableIndexBuildTest.runMultiPhase(rst, {
    positions: {
        [PdibPhase.SCAN]: PdibPosition.MIDDLE,
        [PdibPhase.LOAD]: PdibPosition.MIDDLE,
        [PdibPhase.DRAIN]: PdibPosition.MIDDLE,
    },
});
PrimaryDrivenResumableIndexBuildTest.tearDown(rst);
