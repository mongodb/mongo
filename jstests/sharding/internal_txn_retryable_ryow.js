/*
 * Regression test for the read-your-own-writes property of retryable writes
 * that are rewritten by the router as internal transactions, including the
 * cases that exercise the short-circuit path triggered by a duplicate
 * stmtId in the per-session history.
 *
 * Companion to the TLA+ model at
 * src/mongo/tla_plus/RetryableWrites/RetryableInternalTxnRYOW/. The model
 * proves that a retry that short-circuits before re-executing a
 * non-retryable statement on a still-prepared participant violates RYOW.
 * This test pins the runtime expectation: any retryable write that the
 * client observes as successful must be visible to a follow-up read on
 * the same session, even when the retry hits the duplicate-stmtId
 * short-circuit path.
 *
 * @tags: [requires_fcv_60, uses_transactions]
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 1, mongos: 1, config: 1, rs: {nodes: 1}});
const shard0Primary = st.rs0.getPrimary();

const kDbName = "internal_txn_retryable_ryow_db";
const kCollName = "coll";
const mongosDb = st.s.getDB(kDbName);
const mongosColl = mongosDb.getCollection(kCollName);

assert.commandWorked(mongosDb.createCollection(kCollName));

/**
 * Returns a fresh session-local-id object that uses the internal-transaction
 * shape: an outer logical session uuid plus a txnUUID stamping the internal
 * txn. Reusing the same lsid across retries is how the router signals that
 * the second attempt is a retry of the first.
 */
function makeRetryableInternalLsid() {
    return {id: UUID(), txnUUID: UUID()};
}

/**
 * Builds a session-tagged command body that runs as a retryable write
 * (autocommit is omitted; presence of stmtIds + txnNumber + lsid is enough
 * for the server to treat this as a retryable write).
 */
function withSession(cmdObj, lsid, txnNumber) {
    return Object.assign({}, cmdObj, {lsid: lsid, txnNumber: NumberLong(txnNumber)});
}

function clearColl() {
    assert.commandWorked(mongosColl.remove({}));
}

/**
 * Direct primary read using snapshot-with-no-afterClusterTime to mirror
 * the bug shape: the follow-up read in the ticket does not provide an
 * afterClusterTime >= commitTimestamp. The result is what an in-session
 * read on the same lsid would observe immediately after a retry returns
 * ok.
 */
function readWithoutAfterClusterTime(lsid, txnNumber, docId) {
    const res = assert.commandWorked(
        shard0Primary.getDB(kDbName).runCommand({
            find: kCollName,
            filter: {_id: docId},
            lsid: lsid,
            txnNumber: NumberLong(txnNumber + 1),
        }),
    );
    return res.cursor.firstBatch;
}

// ----------------------------------------------------------------------------
// Test 1: Plain retryable insert. The first attempt succeeds; a retry with the
// same lsid + txnNumber + stmtId must not re-insert and must report the
// statement as retried. A read on the same session must see the document.
// This is the baseline RYOW path the bug threatens to invalidate.
// ----------------------------------------------------------------------------
(function basicRyowSurvivesRetry() {
    jsTest.log("Test 1: retryable insert + same-session retry preserves RYOW");
    clearColl();

    const lsid = makeRetryableInternalLsid();
    const txnNumber = 1;
    const stmtId = NumberInt(0);
    const docId = 1001;

    const initialCmd = withSession(
        {insert: kCollName, documents: [{_id: docId, v: "first"}], stmtIds: [stmtId]},
        lsid,
        txnNumber,
    );
    const initialRes = assert.commandWorked(mongosDb.runCommand(initialCmd));
    assert.eq(initialRes.n, 1, "initial insert should write one doc");
    assert(!initialRes.hasOwnProperty("retriedStmtIds"),
           "initial insert should not be reported as a retry");

    // Retry — same lsid, same txnNumber, same stmtId. Server must recognize
    // this as a retry and not re-execute. Crucially, the client must observe
    // the document on the same session.
    const retryRes = assert.commandWorked(mongosDb.runCommand(initialCmd));
    assert.eq(retryRes.n, 1, "retry should report n=1 mirroring the original");
    assert.eq(retryRes.retriedStmtIds, [stmtId],
              "retry must mark the stmtId as already executed");

    const docs = readWithoutAfterClusterTime(lsid, txnNumber, docId);
    assert.eq(docs.length, 1,
              "RYOW: read on same session must see the retryable write");
    assert.eq(docs[0].v, "first", "RYOW: read must see the originally inserted value");
})();

// ----------------------------------------------------------------------------
// Test 2: Retry that takes the short-circuit code path on a retryable write
// whose statement was already durably recorded in the session history. This
// is the path the ticket flags. With a single-shard ShardingTest the
// "non-retryable participant" case from the ticket cannot be triggered
// end-to-end (it requires two shards in a half-committed two-phase commit),
// so we exercise the runtime contract that survives: the retried write must
// be visible to a follow-up read on the same session, and the retry path
// must NOT report success for a write that is not in fact durable on the
// primary.
// ----------------------------------------------------------------------------
(function retryShortCircuitMustNotBypassDurability() {
    jsTest.log("Test 2: short-circuit response is only legal once the original write is durable");
    clearColl();

    const lsid = makeRetryableInternalLsid();
    const txnNumber = 7;
    const stmtId = NumberInt(0);
    const docId = 2002;

    // Run the write the first time so its stmtId is in the session history on
    // the primary. The mongos response is the "client observed success" point
    // in the model.
    const firstCmd = withSession(
        {insert: kCollName, documents: [{_id: docId, v: "v1"}], stmtIds: [stmtId]},
        lsid,
        txnNumber,
    );
    assert.commandWorked(mongosDb.runCommand(firstCmd));

    // Retry — this is the request that the production bug short-circuits on.
    const retryRes = assert.commandWorked(mongosDb.runCommand(firstCmd));
    assert.eq(retryRes.retriedStmtIds, [stmtId],
              "retry path must surface the duplicate stmtId");

    // Invariant: a successful retry response (even a short-circuited one)
    // means the document is visible to the same session right now, with no
    // afterClusterTime hint required.
    const docs = readWithoutAfterClusterTime(lsid, txnNumber, docId);
    assert.eq(docs.length, 1,
              "RYOW: short-circuited retry response must imply the write is durable");
    assert.eq(docs[0].v, "v1",
              "RYOW: short-circuited retry must reflect the originally inserted value");

    // Belt-and-braces: a direct unsessioned local read on the shard primary
    // must agree with the in-session read.
    const directDocs = shard0Primary.getDB(kDbName).getCollection(kCollName)
                         .find({_id: docId}).toArray();
    assert.eq(directDocs.length, 1,
              "shard primary local read must agree with in-session read");
})();

// ----------------------------------------------------------------------------
// Test 3: Multiple retries of the same retryable write. RYOW must hold across
// every retry — not just the first. Any short-circuit branch that returns
// success without surfacing the durable write is a regression.
// ----------------------------------------------------------------------------
(function repeatedRetriesPreserveRyow() {
    jsTest.log("Test 3: repeated retries on the same lsid all preserve RYOW");
    clearColl();

    const lsid = makeRetryableInternalLsid();
    const txnNumber = 11;
    const stmtId = NumberInt(0);
    const docId = 3003;

    const cmd = withSession(
        {insert: kCollName, documents: [{_id: docId, v: "v3"}], stmtIds: [stmtId]},
        lsid,
        txnNumber,
    );

    const initial = assert.commandWorked(mongosDb.runCommand(cmd));
    assert.eq(initial.n, 1);

    for (let i = 0; i < 3; i++) {
        const r = assert.commandWorked(mongosDb.runCommand(cmd));
        assert.eq(r.retriedStmtIds, [stmtId], `retry attempt ${i} should be marked as retry`);
        const docs = readWithoutAfterClusterTime(lsid, txnNumber, docId);
        assert.eq(docs.length, 1,
                  `RYOW must hold after retry attempt ${i} (got docs=${tojson(docs)})`);
    }
})();

// ----------------------------------------------------------------------------
// Test 4: Sanity — a non-retried write in a sibling session still satisfies
// RYOW with no afterClusterTime. Pins the baseline so future regressions to
// the same code path on the sessioned read side surface cleanly.
// ----------------------------------------------------------------------------
(function nonRetryBaseline() {
    jsTest.log("Test 4: baseline non-retry RYOW sanity");
    clearColl();

    const lsid = makeRetryableInternalLsid();
    const txnNumber = 21;
    const docId = 4004;

    assert.commandWorked(mongosDb.runCommand(withSession(
        {insert: kCollName, documents: [{_id: docId, v: "v4"}], stmtIds: [NumberInt(0)]},
        lsid,
        txnNumber,
    )));

    const docs = readWithoutAfterClusterTime(lsid, txnNumber, docId);
    assert.eq(docs.length, 1, "baseline RYOW must hold");
    assert.eq(docs[0].v, "v4");
})();

st.stop();
