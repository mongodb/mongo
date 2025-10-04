/**
 * Test that an index build aborted due to rollback restarts correctly, even if the none of the
 * associated oplog entries are rolled-back.
 * @tags: [
 *   # We don't need to handle rollbacks in primary-driven index builds.
 *   primary_driven_index_builds_incompatible,
 * ]
 */
import {RollbackIndexBuildsTest} from "jstests/replsets/libs/rollback_index_builds_test.js";

const rollbackIndexTest = new RollbackIndexBuildsTest([ErrorCodes.InterruptedDueToReplStateChange]);

const schedule = [
    // Start an index build.
    "start",
    // Hold the stable timestamp, if applicable. This will prevent the startIndexBuild oplog entry
    // from being rolled-back.
    "holdStableTimestamp",
    // This aborts the active index build.
    "transitionToRollback",
    // Allow the new primary to take over, and let the rolled-back primary restart its index build.
    "transitionToSteadyState",
    // If failover is supported, the new index build will already commit on the new primary. Allow
    // the old primary to finish.
    "commit",
];

rollbackIndexTest.runSchedules([schedule]);
rollbackIndexTest.stop();
