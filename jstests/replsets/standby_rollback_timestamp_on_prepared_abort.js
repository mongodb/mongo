/**
 * SERVER-125802: When a standby applies the abort entry for a prepared
 * transaction, it must advance its rollback timestamp to the abort's optime so
 * that, after a step-up, any checkpoint that observes the abort's effects has
 * a well-defined visibility relationship with the engine's rollback timestamp.
 *
 * Repro shape (without the fix):
 *   1. Primary prepares a txn at prepareTs, then aborts at abortTs.
 *   2. Standby replicates prepare + abort.  abortTs is NOT yet checkpointed.
 *   3. Standby steps up.
 *   4. New primary takes a checkpoint that captures the abort's effects.
 *   5. Because the old standby never advanced rollbackTs at abort-apply, the
 *      engine cannot determine whether the abort is visible in the checkpoint
 *      -> inconsistent post-recovery state for any document the prepared txn
 *      touched.
 *
 * With the fix: at step 2, the standby advances rollbackTs to abortTs, so the
 * checkpoint at step 4 trivially knows the abort is visible.
 *
 * This jstest is the empirical companion to the TLA+ spec at
 *   src/mongo/tla_plus/Replication/RollbackPreparedAbort/RollbackPreparedAbort.tla
 *
 * @tags: [
 *   uses_prepare_transaction,
 *   uses_transactions,
 *   requires_persistence,
 *   requires_majority_read_concern,
 * ]
 */
import {PrepareHelpers} from "jstests/core/txns/libs/prepare_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {reconnect} from "jstests/replsets/rslib.js";

const replTest = new ReplSetTest({nodes: 2});
replTest.startSet();
replTest.initiate();

const dbName = "rollback_prepared_abort_db";
const collName = "rollback_prepared_abort_coll";

let primary = replTest.getPrimary();
let secondary = replTest.getSecondary();

assert.commandWorked(primary.getDB(dbName).runCommand({create: collName, writeConcern: {w: "majority"}}));
const baseDoc = {_id: "base"};
assert.commandWorked(
    primary.getDB(dbName).getCollection(collName).insert(baseDoc, {writeConcern: {w: "majority"}}),
);
replTest.awaitReplication();

jsTestLog("Starting transaction on primary and preparing it");
const session = primary.startSession({causalConsistency: false});
session.startTransaction({writeConcern: {w: "majority"}});
const txnDoc = {_id: "txn-doc"};
assert.commandWorked(session.getDatabase(dbName).getCollection(collName).insert(txnDoc));

const prepareTimestamp = PrepareHelpers.prepareTransaction(session);
jsTestLog("Prepared at ts=" + tojson(prepareTimestamp));
replTest.awaitReplication();

jsTestLog("Aborting the prepared transaction on the primary");
assert.commandWorked(session.abortTransaction_forTesting());
replTest.awaitReplication();

// Capture the secondary's lastApplied optime; the standby's rollbackTimestamp
// is expected to be at least the abort's optime after the abort is applied.
const secondaryStatus = assert.commandWorked(secondary.adminCommand({replSetGetStatus: 1}));
const secondaryLastApplied = secondaryStatus.optimes.appliedOpTime;
jsTestLog("Secondary lastApplied after abort: " + tojson(secondaryLastApplied));

// Read WT rollback timestamp via serverStatus; it must be at least the abort
// optime to satisfy the SERVER-125802 contract.
function getRollbackTimestamp(conn) {
    const ss = assert.commandWorked(conn.adminCommand({serverStatus: 1}));
    // Field path is wiredTiger.snapshot-window-settings or storageEngineStatus
    // depending on build; consult both.
    if (ss.storageEngine && ss.storageEngine.rollbackTimestamp) {
        return ss.storageEngine.rollbackTimestamp;
    }
    if (ss.wiredTiger && ss.wiredTiger["snapshot-window-settings"]) {
        const w = ss.wiredTiger["snapshot-window-settings"];
        if (w["rollback timestamp"]) {
            return w["rollback timestamp"];
        }
    }
    return null;
}

const rbts = getRollbackTimestamp(secondary);
jsTestLog("Secondary rollback timestamp: " + tojson(rbts));
assert(rbts !== null, "Could not read rollback timestamp from serverStatus on secondary");

// Core assertion: the standby's rollback timestamp is >= the abort optime.
// Compare on the ts component since rollbackTimestamp is a BSON Timestamp.
function tsAtLeast(a, b) {
    if (a.t > b.t) return true;
    if (a.t < b.t) return false;
    return a.i >= b.i;
}
assert(
    tsAtLeast(rbts, secondaryLastApplied.ts),
    "Standby did not advance rollbackTimestamp on prepared-abort apply: " +
        "rollbackTs=" +
        tojson(rbts) +
        " < abortOptime=" +
        tojson(secondaryLastApplied.ts),
);

// Now step up the secondary and verify a checkpoint captures the abort's
// effects without crashing the engine into an inconsistent state.
jsTestLog("Stepping up the secondary");
replTest.stepUp(secondary);
reconnect(primary);
primary = replTest.getPrimary();
assert.eq(primary, secondary);

jsTestLog("Forcing a checkpoint on the new primary");
assert.commandWorked(primary.adminCommand({fsync: 1}));

jsTestLog("Verifying the aborted txn's effects are NOT visible post-step-up");
const visibleDocs = primary.getDB(dbName).getCollection(collName).find().toArray();
assert.eq(1, visibleDocs.length, "Expected only the base doc after aborted prepare: " + tojson(visibleDocs));
assert.docEq(baseDoc, visibleDocs[0]);

jsTestLog("Verifying we can run a fresh txn on the new primary post-recovery");
const newSession = primary.startSession({causalConsistency: false});
newSession.startTransaction({writeConcern: {w: "majority"}});
assert.commandWorked(
    newSession.getDatabase(dbName).getCollection(collName).insert({_id: "post-step-up"}),
);
assert.commandWorked(newSession.commitTransaction_forTesting());

replTest.stopSet();
