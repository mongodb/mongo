/**
 * Tests different permutations of rolling-back index build start and commit oplog entries.
 * @tags: [
 *   # We don't need to handle rollbacks in primary-driven index builds.
 *   primary_driven_index_builds_incompatible,
 * ]
 */
import {RollbackIndexBuildsTest} from "jstests/replsets/libs/rollback_index_builds_test.js";

const rollbackIndexTest = new RollbackIndexBuildsTest();

// Build a schedule of operations interleaving rollback and an index build.
const rollbackOps = ["holdStableTimestamp", "transitionToRollback"];
const indexBuildOps = ["start", "commit"];

// This generates 4 choose 2, or 6 schedules.
const schedules = RollbackIndexBuildsTest.makeSchedules(rollbackOps, indexBuildOps);
rollbackIndexTest.runSchedules(schedules);
rollbackIndexTest.stop();
