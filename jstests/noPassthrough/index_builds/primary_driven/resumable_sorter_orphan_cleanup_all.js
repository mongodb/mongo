/**
 * Tests that a resumed primary-driven index build cleans up all sorter entries when no ranges were
 * ever persisted.
 *
 * @tags: [
 *   requires_otel_build,
 *   requires_persistence,
 *   requires_replication,
 * ]
 */
import {PrimaryDrivenResumableIndexBuildTest} from "jstests/noPassthrough/libs/index_builds/primary_driven.js";

const rst = PrimaryDrivenResumableIndexBuildTest.setUp({nodes: 3});
PrimaryDrivenResumableIndexBuildTest.runSorterOrphanCleanup(rst, {skipSpills: 0});
PrimaryDrivenResumableIndexBuildTest.tearDown(rst);
