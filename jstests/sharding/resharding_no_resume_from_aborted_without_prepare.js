/**
 * SERVER-125589: a TransactionParticipant in state kAbortedWithoutPrepare must NOT be restartable
 * at the same txnNumber via TransactionActions::kStartOrContinue (sent on the wire as
 * `startOrContinueTransaction: true' by sub-routers). Restarting silently discards the pre-abort
 * writes and a subsequent commitTransaction persists only the post-restart tail - a textbook
 * silent-data-loss bug.
 *
 * This test exercises the forbidden path directly against a single shard, mirroring the
 * counterexample TLA+ produces from the bug configuration of
 * src/mongo/tla_plus/Sharding/ReshardingAbortRestart:
 *
 *   1. Open (lsid, N) with `startTransaction: true' and insert a "pre-abort" document.
 *   2. abortTransaction - the participant moves to kAbortedWithoutPrepare.
 *   3. Replay the same (lsid, N) with `startOrContinueTransaction: true' and insert a
 *      "post-restart" document.
 *   4. With the SERVER-125589 fix in place, step (3) must fail and the participant must remain
 *      terminal. Without the fix, step (3) silently succeeds and a subsequent commit only
 *      persists the post-restart insert.
 *
 * The companion TLA+ spec covers the state machine exhaustively; this test pins the wire-level
 * behaviour on a live mongod so the model and the runtime cannot drift.
 *
 * @tags: [
 *      requires_fcv_80,
 *      requires_sharding,
 *      uses_transactions,
 * ]
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({mongos: 1, shards: 1});

const dbName = "reshardAbortRestartDb";
const collName = "coll";
const ns = `${dbName}.${collName}`;

assert.commandWorked(st.s0.adminCommand({enableSharding: dbName}));

// Run the participant-level protocol directly against the shard. The bug lives in
// TransactionParticipant, not in mongos routing - using the shard's connection makes the test
// independent of any router-level retry logic that might paper over the defect.
const shardDb = st.getPrimaryShard(dbName).getDB(dbName);
const shardAdmin = st.getPrimaryShard(dbName).getDB("admin");

assert.commandWorked(shardDb.runCommand({create: collName, writeConcern: {w: "majority"}}));

const lsid = {id: UUID()};
const txnNumber = NumberLong(0);

// ----- Step 1: open (lsid, 0) with kStart, insert pre-abort doc. -------------------------------
const preAbortDoc = {_id: "pre-abort", phase: "before_abort"};
assert.commandWorked(
    shardDb.runCommand({
        insert: collName,
        documents: [preAbortDoc],
        lsid: lsid,
        txnNumber: txnNumber,
        stmtId: NumberInt(0),
        startTransaction: true,
        autocommit: false,
    }),
);

// ----- Step 2: abortTransaction; participant -> kAbortedWithoutPrepare. ------------------------
assert.commandWorked(
    shardAdmin.runCommand({
        abortTransaction: 1,
        lsid: lsid,
        txnNumber: txnNumber,
        autocommit: false,
    }),
);

// ----- Step 3: replay (lsid, 0) with kStartOrContinue. Must fail under the fix. ----------------
//
// Pre-fix (SERVER-85170 baseline): this insert silently succeeds because
// `_shouldRestartTransactionOnReuseActiveTxnNumber' returns true for the
// kAbortedWithoutPrepare branch, the participant transitions back to kInProgress at the same
// txnNumber, and the insert is accepted. A subsequent commit would persist ONLY this
// post-restart document - silent loss of the pre-abort insert.
//
// Post-fix (SERVER-125589): the participant refuses to leave kAbortedWithoutPrepare. The shard
// returns a non-OK status - any of TransactionAborted / NoSuchTransaction / TransactionTooOld is
// acceptable, plus the broader IllegalOperation / ConflictingOperationInProgress family in case
// the implementer chose a more specific code. The test asserts only that the command FAILS;
// the exact error code is intentionally loose so the test does not encode a single implementation
// choice.
const postRestartDoc = {_id: "post-restart", phase: "after_revival_attempt"};
const restartAttempt = shardDb.runCommand({
    insert: collName,
    documents: [postRestartDoc],
    lsid: lsid,
    txnNumber: txnNumber,
    stmtId: NumberInt(1),
    startOrContinueTransaction: true,
    autocommit: false,
});

assert.commandFailed(
    restartAttempt,
    "SERVER-125589: kStartOrContinue must NOT restart a kAbortedWithoutPrepare participant " +
        "at the same txnNumber. A successful response here means the participant left the " +
        "aborted terminal state and any subsequent commit will silently drop the pre-abort " +
        "writes. Got: " +
        tojson(restartAttempt),
);

// ----- Step 4: confirm the participant remained terminal. --------------------------------------
//
// commitTransaction on (lsid, 0) must NOT durably persist either document. Per SERVER-125589 the
// participant is aborted-terminal; commit may either be rejected outright or silently no-op,
// depending on how the implementer chose to surface the state. We assert only the outward
// observable: neither document persists.
shardAdmin.runCommand({
    commitTransaction: 1,
    lsid: lsid,
    txnNumber: txnNumber,
    autocommit: false,
});

const persisted = shardDb[collName].find({_id: {$in: ["pre-abort", "post-restart"]}}).toArray();
assert.eq(
    [],
    persisted,
    "SERVER-125589: after kStartOrContinue restart was refused, no writes from (lsid, 0) " +
        "may persist. Found: " + tojson(persisted),
);

// ----- Step 5: a fresh attempt at a new txnNumber must work end-to-end. ------------------------
//
// The point of SERVER-125589's fix is to escalate sub-routers to a NEW txnNumber, not to
// permanently disable the (lsid). Confirm the recovery path: txnNumber N+1 commits normally.
const newTxnNumber = NumberLong(1);
const newDoc = {_id: "new-attempt", phase: "fresh_txn"};
assert.commandWorked(
    shardDb.runCommand({
        insert: collName,
        documents: [newDoc],
        lsid: lsid,
        txnNumber: newTxnNumber,
        stmtId: NumberInt(0),
        startTransaction: true,
        autocommit: false,
    }),
);
assert.commandWorked(
    shardAdmin.runCommand({
        commitTransaction: 1,
        lsid: lsid,
        txnNumber: newTxnNumber,
        autocommit: false,
    }),
);

assert.eq(
    [newDoc],
    shardDb[collName].find({_id: "new-attempt"}).toArray(),
    "SERVER-125589: after escalating to a new txnNumber, the fresh transaction must commit.",
);

st.stop();
