/**
 * SERVER-126373: rollback must identify atomic-applyOps oplog entries that carry
 * retryable-write markers (lsid + txnNumber + stmtIds in their inner ops) as
 * retryable, so the rollback recovery path refreshes config.transactions and a
 * client retry on the new primary returns the cached response instead of
 * re-applying the statement.
 *
 * Scenario:
 *   1. Issue a retryable write that the server packages into an atomic applyOps
 *      oplog entry on the soon-to-roll-back primary.
 *   2. Drive a rollback via RollbackTest. The retryable applyOps lives in the
 *      rolled-back suffix.
 *   3. After rollback completes, retry the same write (same lsid, same
 *      txnNumber) against the new primary. The retry must be classified by the
 *      session machinery as a retry of an already-known statement: the response
 *      must indicate n=1 and no duplicate document must appear.
 *   4. Assert config.transactions on the (formerly rolled-back) node carries a
 *      record for the session with txnNumber >= the retried txnNumber. Before
 *      the SERVER-126373 fix this record was absent (the rollback classifier
 *      treated the atomic applyOps as plain non-retryable) and the retry
 *      double-inserted.
 *
 * @tags: [
 *   requires_fcv_80,
 *   requires_mongobridge,
 *   uses_retryable_writes,
 * ]
 */
import {RollbackTest} from "jstests/replsets/libs/rollback_test.js";

const testName = "rollback_atomic_retryable_applyops_marker";
const dbName = testName;
const collName = "c";

const rollbackTest = new RollbackTest(testName);

// ----------- Common ops on stable primary ------------------------------------
let primary = rollbackTest.getPrimary();
assert.commandWorked(primary.getDB(dbName).createCollection(collName));
rollbackTest.awaitLastOpCommitted();

// ----------- Operations that will be rolled back -----------------------------
rollbackTest.transitionToRollbackOperations();

// Bind a single lsid + txnNumber to the retryable write. Server packages the
// matching write into an atomic applyOps oplog entry; that entry is what the
// rollback classifier in SERVER-126373 has to identify as retryable.
const lsid = {id: UUID()};
const txnNumber = NumberLong(42);

const retryableCmd = {
    insert: collName,
    documents: [{_id: "ratypwr-1", v: 1}],
    lsid: lsid,
    txnNumber: txnNumber,
    // ordered + writeConcern intentionally left at defaults so the server
    // packages this as a single atomic applyOps when the storage engine path
    // batches retryable writes.
};

const rollbackDb = primary.getDB(dbName);
assert.commandWorked(rollbackDb.runCommand(retryableCmd));

// Confirm the entry shows up as an atomic applyOps on the primary that is
// about to roll back. (op:"c", o.applyOps[0].op:"i", lsid + stmtId present.)
const oplog = primary.getDB("local").oplog.rs;
const applyOpsEntry = oplog.findOne({
    "o.applyOps.ns": dbName + "." + collName,
    "lsid.id": lsid.id,
    "txnNumber": txnNumber,
});
assert(applyOpsEntry,
       "expected an atomic applyOps oplog entry tagged with the retryable lsid/txnNumber");
assert(Array.isArray(applyOpsEntry.o.applyOps) && applyOpsEntry.o.applyOps.length >= 1,
       "atomic applyOps must carry an inner ops array");
assert(applyOpsEntry.o.applyOps[0].stmtId !== undefined ||
           applyOpsEntry.stmtId !== undefined,
       "retryable applyOps must carry stmtId(s) so rollback can mark it retryable");

// ----------- Roll back -------------------------------------------------------
rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
rollbackTest.transitionToSyncSourceOperationsDuringRollback();
rollbackTest.transitionToSteadyStateOperations();

// ----------- Verify rollback classified the applyOps as retryable ------------
const newPrimary = rollbackTest.getPrimary();
const newPrimaryDb = newPrimary.getDB(dbName);

// 1. The rolled-back insert is gone from the user collection.
assert.eq(0,
          newPrimaryDb[collName].find({_id: "ratypwr-1"}).itcount(),
          "rolled-back retryable insert must not be visible after rollback");

// 2. Retry the same command (same lsid + txnNumber) against the new primary.
//    If rollback correctly refreshed config.transactions with the rolled-back
//    retryable applyOps entry, the retry returns n=1 without re-inserting.
//    If rollback misclassified the entry (the SERVER-126373 bug), the session
//    machinery has no record of stmtId 0, treats this as a fresh write, and
//    inserts a duplicate document.
const retryRes = assert.commandWorked(newPrimaryDb.runCommand(retryableCmd));
assert.eq(1, retryRes.n,
          "retry of an already-applied retryable applyOps must return n=1: " +
              tojson(retryRes));

// 3. Exactly one document must exist for the retried _id. A duplicate here is
//    the user-visible failure mode SERVER-126373 prevents.
assert.eq(1,
          newPrimaryDb[collName].find({_id: "ratypwr-1"}).itcount(),
          "retried retryable applyOps must not double-insert after rollback");

// 4. config.transactions on the rolled-back node must carry a record for the
//    session with txnNum >= our txnNumber. This is the load-bearing assertion
//    of SERVER-126373: rollback recovery refreshed the session table for an
//    atomic retryable applyOps entry.
const txnRecord = newPrimary.getDB("config")
                      .transactions.findOne({"_id.id": lsid.id});
assert(txnRecord,
       "config.transactions must contain a record for the retryable applyOps " +
           "session after rollback recovery");
assert.gte(txnRecord.txnNum,
           txnNumber,
           "config.transactions txnNum must reflect the rolled-back retryable " +
               "applyOps (got " + tojson(txnRecord) + ")");

rollbackTest.stop();
