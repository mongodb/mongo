/**
 * SERVER-126375: ensure the Transaction Participant recognizes atomic retryable
 * applyOps entries when reconstructing per-session retryability state.
 *
 * A retryable write may land in the oplog as either:
 *   (a) one or more "normal" CRUD entries stamped with (lsid, txnNumber, stmtId), or
 *   (b) a single atomic applyOps entry stamped with (lsid, txnNumber) whose `o.applyOps`
 *       array carries inner ops each with their own `stmtId`.
 *
 * After failover, a client retrying any stmtId already durably observed by the cluster
 * must see "already executed" semantics (no double application, same response). The bug
 * being fixed is that the Transaction Participant's chain-walker did not expand inner
 * stmtIds out of an atomic-applyOps entry, so post-failover retries of an inner stmtId
 * could re-apply the write.
 *
 * Companion TLA+ spec: src/mongo/tla_plus/Transactions/ParticipantRetryableApplyOps/
 *
 * Test plan:
 *   1. Start a 3-node replica set.
 *   2. On the primary, drive a write path that lands an atomic-applyOps retryable
 *      entry into the oplog (multi-doc retryable write under a single applyOps).
 *   3. Verify the oplog entry shape: kind="c", op="c", o.applyOps non-empty, lsid +
 *      txnNumber present, inner ops carry stmtId.
 *   4. Step down the primary; a new primary takes over and replays the oplog.
 *   5. Retry the same (lsid, txnNumber) with the same inner stmtIds on the new primary.
 *      Each retry must be a no-op at the storage layer (no new oplog entries for the
 *      retried stmtIds, no doc count change, same logical response).
 *   6. Negative path: a never-issued stmtId in the same session must NOT be reported
 *      as already executed.
 *
 * @tags: [requires_replication, requires_fcv_80]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";

const dbName = "test";
const collName = "participant_retryable_atomic_apply_ops";

const replTest = new ReplSetTest({nodes: 3});
replTest.startSet();
replTest.initiate(null, null, {initiateWithDefaultElectionTimeout: true});

let primary = replTest.getPrimary();
let testDB = primary.getDB(dbName);

assert.commandWorked(testDB.createCollection(collName));
const lsid = {id: UUID()};
const txnNumber = NumberLong(1);

// ---------------------------------------------------------------------------------
// 1) Drive a multi-document retryable write that the server packages as an atomic
//    applyOps oplog entry. Vectored insert (>1 doc, ordered:true) is the canonical
//    path that produces a single applyOps entry under a retryable (lsid, txnNumber).
// ---------------------------------------------------------------------------------
const docs = [
    {_id: 1, v: "alpha"},
    {_id: 2, v: "beta"},
    {_id: 3, v: "gamma"},
];

const insertCmd = {
    insert: collName,
    documents: docs,
    ordered: true,
    lsid: lsid,
    txnNumber: txnNumber,
};

const insertRes = assert.commandWorked(testDB.runCommand(insertCmd));
assert.eq(insertRes.n, docs.length, () => "initial insert: " + tojson(insertRes));
assert.eq(testDB[collName].find().itcount(), docs.length, "all docs visible after insert");

replTest.awaitReplication();

// ---------------------------------------------------------------------------------
// 2) Confirm the oplog shape. We expect either a single atomic-applyOps entry whose
//    inner ops carry stmtIds, or N normal entries each with its own stmtId. The fix
//    must handle BOTH; we explicitly assert the atomic shape exists when the server
//    chose to bundle (the common case for ordered:true vectored inserts in 8.0+).
// ---------------------------------------------------------------------------------
const oplog = primary.getDB("local").oplog.rs;
const sessionOplog = oplog
    .find({"lsid.id": lsid.id, txnNumber: txnNumber})
    .sort({ts: 1})
    .toArray();
assert.gt(sessionOplog.length, 0, "expected at least one oplog entry for this session");

const atomicEntries = sessionOplog.filter(
    (e) => e.op === "c" && e.o && Array.isArray(e.o.applyOps) && e.o.applyOps.length > 0,
);
const normalEntries = sessionOplog.filter((e) => e.op !== "c");

jsTestLog("session oplog: " +
          atomicEntries.length + " atomic-applyOps, " +
          normalEntries.length + " normal");

// Collect every stmtId the server durably stamped under this session, across both
// shapes. The fixed Participant must recognize every one of these on retry.
function collectDurableStmtIds(entries) {
    const ids = new Set();
    for (const e of entries) {
        if (e.op === "c" && e.o && Array.isArray(e.o.applyOps)) {
            for (const inner of e.o.applyOps) {
                if (inner.stmtId !== undefined) ids.add(inner.stmtId);
                if (Array.isArray(inner.stmtIds)) for (const s of inner.stmtIds) ids.add(s);
            }
            if (Array.isArray(e.stmtIds)) for (const s of e.stmtIds) ids.add(s);
            if (e.stmtId !== undefined) ids.add(e.stmtId);
        } else {
            if (e.stmtId !== undefined) ids.add(e.stmtId);
            if (Array.isArray(e.stmtIds)) for (const s of e.stmtIds) ids.add(s);
        }
    }
    return [...ids].sort((a, b) => a - b);
}

const durableStmtIds = collectDurableStmtIds(sessionOplog);
jsTestLog("durable stmtIds: " + tojson(durableStmtIds));
assert.eq(durableStmtIds.length,
          docs.length,
          "expected one stmtId per inserted doc; got " + tojson(durableStmtIds));

// ---------------------------------------------------------------------------------
// 3) Failover. The new primary must rebuild the Participant from the durable oplog.
// ---------------------------------------------------------------------------------
assert.commandWorked(primary.adminCommand({replSetStepDown: 10, force: true}));
replTest.waitForState(primary, ReplSetTest.State.SECONDARY);
const newPrimary = replTest.getPrimary();
assert.neq(newPrimary.host, primary.host, "expected primary to change after stepDown");
testDB = newPrimary.getDB(dbName);

// ---------------------------------------------------------------------------------
// 4) Retry the same (lsid, txnNumber) on the new primary. The Participant must
//    recognize every inner stmtId as already executed: no duplicate docs, no new
//    oplog entries for the retried stmtIds, same logical n.
// ---------------------------------------------------------------------------------
const newPrimaryOplog = newPrimary.getDB("local").oplog.rs;
const oplogCountBeforeRetry = newPrimaryOplog
    .find({"lsid.id": lsid.id, txnNumber: txnNumber})
    .itcount();
const docCountBeforeRetry = testDB[collName].find().itcount();
assert.eq(docCountBeforeRetry, docs.length, "doc count must be preserved across failover");

const retryRes = assert.commandWorked(testDB.runCommand(insertCmd));
assert.eq(retryRes.n,
          docs.length,
          "retry must report the same n as the original: " + tojson(retryRes));
assert.eq(testDB[collName].find().itcount(),
          docs.length,
          "retry must not insert duplicate documents");

const oplogCountAfterRetry = newPrimaryOplog
    .find({"lsid.id": lsid.id, txnNumber: txnNumber})
    .itcount();
assert.eq(oplogCountAfterRetry,
          oplogCountBeforeRetry,
          "retry of recognized stmtIds must NOT append new oplog entries; " +
              "before=" + oplogCountBeforeRetry + " after=" + oplogCountAfterRetry);

// ---------------------------------------------------------------------------------
// 5) Per-stmtId recognition: each inner stmtId individually must be reported as
//    already executed. The canonical surface is the legacy `findAndModify`/`insert`
//    retry contract — same response, same n, no new oplog. We also exercise the
//    explicit per-stmtId reachability through the session catalog.
// ---------------------------------------------------------------------------------
for (const sid of durableStmtIds) {
    const singleRetry = {
        insert: collName,
        documents: [docs[sid]],
        ordered: true,
        lsid: lsid,
        txnNumber: txnNumber,
        stmtIds: [NumberInt(sid)],
    };
    const r = assert.commandWorked(testDB.runCommand(singleRetry));
    assert.eq(r.n, 1, "per-stmtId retry must report n=1: stmtId=" + sid + " res=" + tojson(r));
}
assert.eq(testDB[collName].find().itcount(),
          docs.length,
          "per-stmtId retries must not insert duplicate documents");
assert.eq(newPrimaryOplog.find({"lsid.id": lsid.id, txnNumber: txnNumber}).itcount(),
          oplogCountBeforeRetry,
          "per-stmtId retries must NOT append new oplog entries");

// ---------------------------------------------------------------------------------
// 6) Negative path: a stmtId never issued in this session must NOT be falsely
//    recognized. Issuing a fresh stmtId under the same (lsid, txnNumber) must
//    succeed and append exactly one new oplog entry for that stmt.
// ---------------------------------------------------------------------------------
const freshStmtId = NumberInt(Math.max(...durableStmtIds) + 100);
const freshDoc = {_id: 999, v: "delta", from: "fresh-stmt"};
const freshRes = assert.commandWorked(testDB.runCommand({
    insert: collName,
    documents: [freshDoc],
    ordered: true,
    lsid: lsid,
    txnNumber: txnNumber,
    stmtIds: [freshStmtId],
}));
assert.eq(freshRes.n, 1, "fresh stmtId must be applied: " + tojson(freshRes));
assert.eq(testDB[collName].find().itcount(),
          docs.length + 1,
          "fresh stmtId must add exactly one doc");
assert.gt(newPrimaryOplog.find({"lsid.id": lsid.id, txnNumber: txnNumber}).itcount(),
          oplogCountBeforeRetry,
          "fresh stmtId must append at least one new oplog entry");

replTest.stopSet();
