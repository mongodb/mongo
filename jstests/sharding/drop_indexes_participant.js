/**
 * Tests the _shardsvrDropIndexesParticipant internal command.
 *
 * Verifies that:
 * - The command actually drops indexes on the target shard
 * - Stale transaction numbers are properly rejected
 *
 * @tags: [
 *  requires_fcv_82
 * ]
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";

function assertShardHasIndexes(st, dbName, collName, expectedIndexNames) {
    let indexes = st.rs0.getPrimary().getDB(dbName)[collName].getIndexes();
    assert.sameMembers(indexes.map(ix => ix.name), expectedIndexNames, tojson(indexes));
}

const st = new ShardingTest({shards: 1});
const dbName = "TestDB";
const collName = "TestColl";
const ns = dbName + "." + collName;
const db = st.s.getDB(dbName);

assert.commandWorked(db.adminCommand({shardCollection: ns, key: {_id: 1}}));
assert.commandWorked(db.TestColl.createIndex({x: 1}));
assertShardHasIndexes(st, dbName, collName, ["_id_", "x_1"]);

const lsid = {
    id: UUID()
};
const txnNumber = NumberLong(1);
const txnParams = {
    lsid: lsid,
    txnNumber: txnNumber,
    writeConcern: {w: "majority"}
};

// --- Test 1: Command successfully drops specified indexes ---
let dropCmd =
    {_shardsvrDropIndexesParticipant: collName, index: "x_1", dbName: dbName, ...txnParams};

assert.commandWorked(st.shard0.getDB(dbName).runCommand(dropCmd));
assertShardHasIndexes(st, dbName, collName, ["_id_"]);

// --- Test 2: Stale transaction number is rejected ---

// Re-create the dropped indexes
assert.commandWorked(st.shard0.getDB(dbName)[collName].createIndex({x: 1}));
assertShardHasIndexes(st, dbName, collName, ["_id_", "x_1"]);

// Fresh txn; increments txnNumber
let newTxnNumber = NumberLong(10);
let freshCmd = Object.assign({}, dropCmd, {txnNumber: newTxnNumber});
assert.commandWorked(st.shard0.getDB(dbName).runCommand(freshCmd));
assertShardHasIndexes(st, dbName, collName, ["_id_"]);

// Recreate indexes again
assert.commandWorked(st.shard0.getDB(dbName)[collName].createIndex({x: 1}));
assertShardHasIndexes(st, dbName, collName, ["_id_", "x_1"]);

let staleCmd = Object.assign({}, dropCmd, {txnNumber: NumberLong(9)});
let res = st.shard0.getDB(dbName).runCommand(staleCmd);

assert.commandFailedWithCode(
    res, ErrorCodes.TransactionTooOld, "Should fail with TransactionTooOld for stale txnNumber");
assertShardHasIndexes(st, dbName, collName, ["_id_", "x_1"]);

st.stop();
