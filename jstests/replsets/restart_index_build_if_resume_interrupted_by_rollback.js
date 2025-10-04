/**
 * Tests that an index build is resumable only once across rollbacks. If the resumed index build
 * fails to run to completion before a subsequent rollback, it will restart from the beginning.
 *
 * @tags: [
 *   # Primary-driven index builds aren't resumable.
 *   primary_driven_index_builds_incompatible,
 *   requires_majority_read_concern,
 *   requires_persistence,
 * ]
 */
import {RollbackResumableIndexBuildTest} from "jstests/replsets/libs/rollback_resumable_index_build.js";
import {RollbackTest} from "jstests/replsets/libs/rollback_test.js";

const dbName = "test";

const rollbackTest = new RollbackTest(jsTestName());

RollbackResumableIndexBuildTest.runResumeInterruptedByRollback(
    rollbackTest,
    dbName,
    [{a: 1}, {a: 2}],
    {a: 1},
    [{a: 3}],
    [{a: 4}],
);

rollbackTest.stop();
