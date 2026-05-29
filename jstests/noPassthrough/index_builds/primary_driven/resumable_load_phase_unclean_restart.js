/**
 * Tests resuming a primary-driven index build in the load phase, with the old primary uncleanly
 * restarted across each failover.
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
    PrimaryDrivenResumableIndexBuildTest,
} from "jstests/noPassthrough/libs/index_builds/primary_driven.js";

const rst = PrimaryDrivenResumableIndexBuildTest.setUp({nodes: 3});
PrimaryDrivenResumableIndexBuildTest.run(rst, {
    phase: PdibPhase.LOAD,
    failoverMode: PdibFailoverMode.UNCLEAN_RESTART,
});
PrimaryDrivenResumableIndexBuildTest.tearDown(rst);
