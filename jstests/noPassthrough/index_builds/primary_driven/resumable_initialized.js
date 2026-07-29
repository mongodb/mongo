/**
 * Tests resuming a primary-driven index build that has been initialized but has not yet started the
 * scan phase.
 *
 * @tags: [
 *   requires_otel_build,
 *   requires_persistence,
 *   requires_replication,
 * ]
 */

import {PrimaryDrivenResumableIndexBuildTest} from "jstests/noPassthrough/libs/index_builds/primary_driven.js";

const rst = PrimaryDrivenResumableIndexBuildTest.setUp();
PrimaryDrivenResumableIndexBuildTest.runSynthesizedResumeState(rst);
PrimaryDrivenResumableIndexBuildTest.tearDown(rst);
