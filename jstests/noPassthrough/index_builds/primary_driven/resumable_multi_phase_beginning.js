/**
 * Tests resuming a single primary-driven index build multiple times in a row -- once at the
 * beginning of each phase (scan, load, drain).
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
        [PdibPhase.SCAN]: PdibPosition.BEGINNING,
        [PdibPhase.LOAD]: PdibPosition.BEGINNING,
        [PdibPhase.DRAIN]: PdibPosition.BEGINNING,
    },
});
PrimaryDrivenResumableIndexBuildTest.tearDown(rst);
