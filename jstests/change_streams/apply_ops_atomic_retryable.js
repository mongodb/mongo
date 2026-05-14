/**
 * Tests that a change stream correctly recognizes an atomic retryable applyOps
 * (one issued with an lsid + txnNumber, but executed as a single atomic applyOps
 * rather than a multi-statement transaction) and unwinds its inner ops as
 * individual change events tagged with the originating lsid/txnNumber.
 *
 * Regression test for SERVER-126374: prior to the fix, atomic retryable applyOps
 * entries were not classified as part of a retryable write, so the unwind path
 * dropped the lsid/txnNumber stamping that downstream consumers rely on for
 * idempotent resumption.
 *
 * @tags: [
 *   change_stream_does_not_expect_txns,
 *   requires_fcv_80,
 *   requires_majority_read_concern,
 *   requires_replication,
 * ]
 */

import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {ChangeStreamTest} from "jstests/libs/query/change_stream_util.js";

// applyOps is only directly issuable against a replica-set primary, not mongos.
if (FixtureHelpers.isMongos(db)) {
    jsTestLog("Skipping apply_ops_atomic_retryable.js on mongos: applyOps is not routable.");
    quit();
}

const collName = "change_stream_apply_ops_atomic_retryable";
const coll = assertDropAndRecreateCollection(db, collName);

const testStartTime = db.runCommand({hello: 1}).$clusterTime.clusterTime;
testStartTime.i++;

const cst = new ChangeStreamTest(db);
const changeStream = cst.startWatchingChanges({
    pipeline: [{$changeStream: {}}, {$project: {"lsid.uid": 0}}],
    collection: coll,
    doNotModifyInPassthroughs: true,
});

// Build a retryable-write session. The session id + monotonically-advanced
// txnNumber are what mark the applyOps as a *retryable* atomic applyOps when
// passed through the runCommand below.
const session = db.getMongo().startSession({causalConsistency: false, retryWrites: true});
const lsid = session.getSessionId();
const txnNumber = NumberLong(session.getTxnNumber_forTesting() + 1);

// Issue an atomic applyOps with the retryable-write envelope (lsid + txnNumber
// + per-op stmtIds). The fix under SERVER-126374 requires the change-stream
// unwind layer to treat this as a retryable applyOps and stamp the resulting
// events with lsid/txnNumber, even though no multi-statement transaction was
// started on the server side.
const applyOpsRes = assert.commandWorked(db.runCommand({
    applyOps: [
        {op: "i", ns: coll.getFullName(), o: {_id: 1, a: "retryable-applyOps-1"}, stmtId: 0},
        {op: "i", ns: coll.getFullName(), o: {_id: 2, a: "retryable-applyOps-2"}, stmtId: 1},
    ],
    lsid: lsid,
    txnNumber: txnNumber,
    // No autocommit:false / startTransaction:true -- this is an atomic applyOps,
    // NOT a transaction.
}));
assert.eq(applyOpsRes.applied, 2, () => tojson(applyOpsRes));

// A non-retryable insert outside the retryable applyOps so we can confirm the
// stream does NOT incorrectly stamp non-retryable events with lsid/txnNumber.
assert.commandWorked(coll.insert({_id: 3, a: "plain-insert"}));

// Drop -> invalidate, terminating the single-collection stream cleanly.
assert.commandWorked(db.runCommand({drop: coll.getName()}));

// Expected change events. The two inserts produced by the atomic retryable
// applyOps MUST carry lsid + txnNumber; the plain insert MUST NOT.
const expectedChanges = [
    {
        documentKey: {_id: 1},
        fullDocument: {_id: 1, a: "retryable-applyOps-1"},
        ns: {db: db.getName(), coll: coll.getName()},
        operationType: "insert",
        lsid: lsid,
        txnNumber: txnNumber,
    },
    {
        documentKey: {_id: 2},
        fullDocument: {_id: 2, a: "retryable-applyOps-2"},
        ns: {db: db.getName(), coll: coll.getName()},
        operationType: "insert",
        lsid: lsid,
        txnNumber: txnNumber,
    },
    {
        documentKey: {_id: 3},
        fullDocument: {_id: 3, a: "plain-insert"},
        ns: {db: db.getName(), coll: coll.getName()},
        operationType: "insert",
    },
    {
        operationType: "drop",
        ns: {db: db.getName(), coll: coll.getName()},
    },
];

cst.assertNextChangesEqualWithDeploymentAwareness({
    cursor: changeStream,
    expectedChanges: expectedChanges,
});

cst.assertNextChangesEqualWithDeploymentAwareness({
    cursor: changeStream,
    expectedChanges: [{operationType: "invalidate"}],
    expectInvalidate: true,
});

// Resumability: a stream opened with `startAtOperationTime` set to before the
// atomic retryable applyOps must see both unwound inserts in the same order
// and with the same lsid/txnNumber stamping. This is the load-bearing claim
// for downstream consumers that resume after a retryable-write retry.
const recreatedColl = assertDropAndRecreateCollection(db, collName);
const resumeStartTime = db.runCommand({hello: 1}).$clusterTime.clusterTime;

const session2 = db.getMongo().startSession({causalConsistency: false, retryWrites: true});
const lsid2 = session2.getSessionId();
const txnNumber2 = NumberLong(session2.getTxnNumber_forTesting() + 1);

assert.commandWorked(db.runCommand({
    applyOps: [
        {op: "i", ns: recreatedColl.getFullName(), o: {_id: 10, a: "resume-1"}, stmtId: 0},
        {op: "i", ns: recreatedColl.getFullName(), o: {_id: 11, a: "resume-2"}, stmtId: 1},
    ],
    lsid: lsid2,
    txnNumber: txnNumber2,
}));

const resumeCst = new ChangeStreamTest(db);
const resumeStream = resumeCst.startWatchingChanges({
    pipeline: [
        {$changeStream: {startAtOperationTime: resumeStartTime}},
        {$project: {"lsid.uid": 0}},
    ],
    collection: recreatedColl,
});

resumeCst.assertNextChangesEqualWithDeploymentAwareness({
    cursor: resumeStream,
    expectedChanges: [
        {
            documentKey: {_id: 10},
            fullDocument: {_id: 10, a: "resume-1"},
            ns: {db: db.getName(), coll: recreatedColl.getName()},
            operationType: "insert",
            lsid: lsid2,
            txnNumber: txnNumber2,
        },
        {
            documentKey: {_id: 11},
            fullDocument: {_id: 11, a: "resume-2"},
            ns: {db: db.getName(), coll: recreatedColl.getName()},
            operationType: "insert",
            lsid: lsid2,
            txnNumber: txnNumber2,
        },
    ],
});

resumeCst.cleanUp();
cst.cleanUp();
