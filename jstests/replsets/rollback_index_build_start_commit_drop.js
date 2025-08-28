/**
 * Tests different permutations of rolling-back index build start, commit, and drop oplog entries.
 * @tags: [
 *   # We don't need to handle rollbacks in primary-driven index builds.
 *   primary_driven_index_builds_incompatible,
 * ]
 */
import {RollbackIndexBuildsTest} from "jstests/replsets/libs/rollback_index_builds_test.js";

const rollbackIndexTest = new RollbackIndexBuildsTest();

// Build a schedule of operations interleaving rollback and an index build.
const rollbackOps = ["holdStableTimestamp", "transitionToRollback"];
const indexBuildOps = ["start", "commit", "drop"];

// This generates 5 choose 3, or 10 schedules.
const schedules = RollbackIndexBuildsTest.makeSchedules(rollbackOps, indexBuildOps);
rollbackIndexTest.runSchedules(schedules);
rollbackIndexTest.stop();
