/**
 * Tests resuming a primary-driven index build in the scan phase when the build contains one index
 * whose sorter spills to disk during the scan and two indexes whose sorters stay entirely in
 * memory.
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
    PrimaryDrivenResumableIndexBuildTest,
} from "jstests/noPassthrough/libs/index_builds/primary_driven.js";

const rst = PrimaryDrivenResumableIndexBuildTest.setUp();
PrimaryDrivenResumableIndexBuildTest.run(rst, {
    phase: PdibPhase.SCAN,
    docTemplate: mixedSpillingDocTemplate,
});
PrimaryDrivenResumableIndexBuildTest.tearDown(rst);
