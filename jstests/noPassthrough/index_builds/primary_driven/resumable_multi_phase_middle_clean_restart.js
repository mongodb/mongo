/**
 * Tests resuming a single primary-driven index build multiple times in a row -- once in the middle
 * of each phase (scan, load, drain), with the old primary cleanly restarted across each failover.
 *
 * @tags: [
 *   requires_otel_build,
 *   requires_persistence,
 *   requires_replication,
 * ]
 */

import {
    PdibFailoverMode,
    PdibPhase,
    PdibPosition,
    PrimaryDrivenResumableIndexBuildTest,
} from "jstests/noPassthrough/libs/index_builds/primary_driven.js";

const rst = PrimaryDrivenResumableIndexBuildTest.setUp({nodes: 3});
PrimaryDrivenResumableIndexBuildTest.runMultiPhase(rst, {
    positions: {
        [PdibPhase.SCAN]: PdibPosition.MIDDLE,
        [PdibPhase.LOAD]: PdibPosition.MIDDLE,
        [PdibPhase.DRAIN]: PdibPosition.MIDDLE,
    },
    failoverMode: PdibFailoverMode.CLEAN_RESTART,
});
PrimaryDrivenResumableIndexBuildTest.tearDown(rst);
