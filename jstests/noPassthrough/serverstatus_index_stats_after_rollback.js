/**
 * Tests that the serverStatus indexStats remains accurate after a replication rollback.
 *
 * @tags: [
 *   requires_replication,
 *   requires_mongobridge,
 * ]
 */
import {assertCountIncrease, assertFeatureCountIncrease, assertStats} from "jstests/libs/index_stats_utils.js";
import {RollbackTest} from "jstests/replsets/libs/rollback_test.js";

const dbName = "test";
const collName = "index_stats_rollback";

// ---------------------------------------------------------------------------
// Test setup — RollbackTest fixture (3-node replica set with mongobridge)
// ---------------------------------------------------------------------------

const rollbackTest = new RollbackTest(jsTestName());

const primary = rollbackTest.getPrimary();
const testDB = primary.getDB(dbName);
const coll = testDB[collName];

// Capture stats before creating the collection and indexes.
let lastStats = testDB.serverStatus().indexStats;

// Create the collection and two secondary indexes during steady-state. The RollbackTest fixture
// ensures these are replicated before transitioning to rollback operations.
// Note: RollbackTest stops replication on the tiebreaker node at setup time, so we must use
// commitQuorum: "majority" (2 of 3) instead of the default "votingMembers" (all 3).
assert.commandWorked(coll.insert({a: 1, b: 1}));
assert.commandWorked(
    testDB.runCommand({createIndexes: collName, indexes: [{key: {a: 1}, name: "a_1"}], commitQuorum: "majority"}),
);
assert.commandWorked(
    testDB.runCommand({createIndexes: collName, indexes: [{key: {b: 1}, name: "b_1"}], commitQuorum: "majority"}),
);

// Verify indexStats.count increased by 3 (_id + {a:1} + {b:1}).
assertStats(testDB, (stats) => {
    assertCountIncrease(lastStats, stats, 3);
    assertFeatureCountIncrease(lastStats, stats, "id", 1);
    assertFeatureCountIncrease(lastStats, stats, "single", 2);
    assertFeatureCountIncrease(lastStats, stats, "normal", 2);
});

// Snapshot the stats right after index creation — this is our reference point.
const statsAfterCreate = testDB.serverStatus().indexStats;
jsTestLog("indexStats after index creation: " + tojson(statsAfterCreate));

// ---------------------------------------------------------------------------
// Rollback: only document inserts are rolled back; indexes stay.
// ---------------------------------------------------------------------------

// Transition to rollback operations — isolate the primary so subsequent writes will be rolled
// back. We only insert documents here; we do NOT create or drop indexes.
const rollbackNode = rollbackTest.transitionToRollbackOperations();
assert.commandWorked(rollbackNode.getDB(dbName)[collName].insert({a: 100, b: 200}));
assert.commandWorked(rollbackNode.getDB(dbName)[collName].insert({a: 101, b: 201}));
assert.commandWorked(rollbackNode.getDB(dbName)[collName].insert({a: 102, b: 202}));

// Elect the other secondary as the new primary and perform a diverging write.
const syncSource = rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
assert.commandWorked(syncSource.getDB(dbName)[collName].insert({a: 999, b: 999}));

// Reconnect the rolled-back node so rollback begins.
rollbackTest.transitionToSyncSourceOperationsDuringRollback();

// Wait for rollback to complete and return to steady state.
rollbackTest.transitionToSteadyStateOperations();

// ---------------------------------------------------------------------------
// Verify indexStats remain correct across rollback.
// ---------------------------------------------------------------------------

// The rolled-back node is now a secondary. Its indexes should still be intact because we only
// rolled back document inserts, not the createIndex operations.
assertStats(rollbackNode.getDB(dbName), (stats) => {
    // Count should be identical to what we recorded before rollback.
    assertCountIncrease(statsAfterCreate, stats, 0);
    assertFeatureCountIncrease(statsAfterCreate, stats, "id", 0);
    assertFeatureCountIncrease(statsAfterCreate, stats, "single", 0);
    assertFeatureCountIncrease(statsAfterCreate, stats, "normal", 0);
});

// Also verify on the current primary (the former sync source).
const newPrimary = rollbackTest.getPrimary();
assertStats(newPrimary.getDB(dbName), (stats) => {
    assertCountIncrease(statsAfterCreate, stats, 0);
    assertFeatureCountIncrease(statsAfterCreate, stats, "id", 0);
    assertFeatureCountIncrease(statsAfterCreate, stats, "single", 0);
    assertFeatureCountIncrease(statsAfterCreate, stats, "normal", 0);
});

// Verify the collection-level index count is still 3 on both nodes.
assert.eq(
    3,
    assert.commandWorked(rollbackNode.getDB(dbName).runCommand({collStats: collName})).nindexes,
    "Expected 3 indexes on rolled-back node after rollback",
);
assert.eq(
    3,
    assert.commandWorked(newPrimary.getDB(dbName).runCommand({collStats: collName})).nindexes,
    "Expected 3 indexes on new primary after rollback",
);

rollbackTest.stop();
