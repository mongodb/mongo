/**
 * Test that rolling back an index build and collection creation behaves correctly.
 * @tags: [
 *   # We don't need to handle rollbacks in primary-driven index builds.
 *   primary_driven_index_builds_incompatible,
 * ]
 */
import {RollbackIndexBuildsTest} from "jstests/replsets/libs/rollback_index_builds_test.js";

const rollbackIndexTest = new RollbackIndexBuildsTest([ErrorCodes.InterruptedDueToReplStateChange]);

const schedule = [
    // Hold the stable timestamp, if applicable.
    "holdStableTimestamp",
    // Everything after this will be rolled-back.
    "transitionToRollback",
    // This creates the collection and starts the index build, so both the collection creation and
    // index build will be rolled-back.
    "start",
    // Allow the rollback to proceed.
    "transitionToSteadyState",
    // Allow the index build to proceed on the rolling-back node.
    "commit",
];

rollbackIndexTest.runSchedules([schedule]);
rollbackIndexTest.stop();
