/**
 * Tests resuming a single primary-driven index build multiple times in a row -- once in the middle
 * of each phase (scan, load, drain) -- when the build contains one index whose sorter spills to
 * disk during the scan and two indexes whose sorters stay entirely in memory.
 *
 * @tags: [
 *   requires_otel_build,
 *   requires_persistence,
 *   requires_replication,
 * ]
 */

import {
    mixedSpillingDocTemplate,
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
    docTemplate: mixedSpillingDocTemplate,
});
PrimaryDrivenResumableIndexBuildTest.tearDown(rst);
