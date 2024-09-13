/**
 * This workload tests the behavior of attempting concurrent compaction on the same collection. When
 * attempting to start parallel compaction on the same collection, a failure is expected as this
 * behaviour is not allowed.
 *
 * @tags: [
 *     requires_compact,
 *     requires_persistence,
 *     requires_wiredtiger,
 * ]
 */

import {configureFailPoint, kDefaultWaitForFailPointTimeout} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";

const conn = MongoRunner.runMongod();

const adminDb = conn.getDB("admin");
const db = conn.getDB("test");

assert.commandWorked(db.createCollection("a"));
assert.commandWorked(db.createCollection("b"));

const fp = configureFailPoint(adminDb, "pauseCompactCommandBeforeWTCompact");
function runCompact(dbName, collectionName) {
    assert.commandWorked(db.getSiblingDB(dbName).runCommand({compact: collectionName}));
}

// Start compaction on "a".
const awaitCompactA = startParallelShell(funWithArgs(runCompact, "test", "a"), conn.port);

assert.commandWorked(adminDb.runCommand({
    waitForFailPoint: "pauseCompactCommandBeforeWTCompact",
    timesEntered: 1,
    maxTimeMS: kDefaultWaitForFailPointTimeout
}));

// Compaction already in progress on "a".
assert.commandFailedWithCode(db.runCommand({compact: "a"}), ErrorCodes.OperationFailed);

// No compaction in progress on "b".
const awaitCompactB = startParallelShell(funWithArgs(runCompact, "test", "b"), conn.port);

assert.commandWorked(adminDb.runCommand({
    waitForFailPoint: "pauseCompactCommandBeforeWTCompact",
    timesEntered: 2,
    maxTimeMS: kDefaultWaitForFailPointTimeout
}));

fp.off();

awaitCompactA();
awaitCompactB();

// No compaction in progress on "a" anymore.
assert.commandWorked(db.runCommand({compact: "a"}));

MongoRunner.stopMongod(conn);
