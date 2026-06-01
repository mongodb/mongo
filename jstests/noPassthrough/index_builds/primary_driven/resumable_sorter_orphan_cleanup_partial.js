/**
 * Tests that a resumed primary-driven index build cleans up orphaned sorter entries that fall
 * outside any persisted ranges.
 *
 * @tags: [
 *   requires_otel_build,
 *   requires_persistence,
 *   requires_replication,
 * ]
 */
import {PrimaryDrivenResumableIndexBuildTest} from "jstests/noPassthrough/libs/index_builds/primary_driven.js";

const rst = PrimaryDrivenResumableIndexBuildTest.setUp({nodes: 3});
PrimaryDrivenResumableIndexBuildTest.runSorterOrphanCleanup(rst, {skipSpills: 1});
PrimaryDrivenResumableIndexBuildTest.tearDown(rst);
