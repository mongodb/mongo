/**
 * SERVER-79952 reproducer.
 *
 * FLE2/QE batched inserts emit auxiliary writes (ESC + ECOC metadata + __safeContent__
 * pull-update) for each user-supplied document. Per the retryable-internal-transactions
 * contract documented in src/mongo/db/s/README_sessions_and_transactions.md, those
 * auxiliary write statements MUST be tagged with stmtId = kUninitializedStmtId (-1) so
 * that retried writes do not re-apply them and so that user-supplied stmtIds are never
 * collided.
 *
 * The current implementation in src/mongo/db/fle_crud.cpp respects the first
 * caller-supplied stmtId of the batch, then increments that "base" stmtId for every
 * generated auxiliary write -- so a request with documents [op1, op2] and stmtIds [1, 3]
 * produces persisted stmtIds [1, 2, 3, ...] for op1's writes and [4, 5, 6, ...] for op2's,
 * silently overwriting op2's caller-supplied stmtId of 3.
 *
 * This test sends an FLE2 batched insert with two encrypted documents and explicit
 * stmtIds [1, 3], then inspects the resulting applyOps oplog entry on the primary. The
 * contract requires:
 *   (C1) Exactly two operation entries carry stmtIds matching the caller-supplied
 *        sequence {1, 3}, one per client document.
 *   (C2) Every other operation entry (ESC/ECOC/safeContent auxiliary writes) carries no
 *        stmtId field at all (the kUninitializedStmtId sentinel is stripped by the
 *        applyOps writer).
 *   (C3) No two operation entries share the same stmtId.
 *
 * Under the buggy implementation this test fails at (C1) -- op2's stored stmtId is 4, not
 * 3 -- and at (C2) -- aux entries carry stmtId 2, 4, 5, etc.
 *
 * @tags: [
 *   requires_fcv_80,
 *   uses_transactions,
 *   assumes_balancer_off,
 * ]
 */
import {EncryptedClient, isEnterpriseShell} from "jstests/fle2/libs/encrypted_client_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {getOplogEntriesForTxn} from "jstests/sharding/libs/sharded_transactions_helpers.js";

if (!isEnterpriseShell()) {
    jsTest.log("Skipping: not enterprise shell (FLE2 requires enterprise).");
    quit();
}

TestData.replicaSetEndpointIncompatible = true;

const kDbName = "fle2_stmt_id_contract";
const kCollName = "encrypted";

const st = new ShardingTest({shards: 1});
const mongos = st.s;

const client = new EncryptedClient(mongos, kDbName);

assert.commandWorked(client.createEncryptionCollection(kCollName, {
    encryptedFields: {
        "fields": [{
            "path": "ssn",
            "bsonType": "string",
            "queries": {"queryType": "equality"},
        }],
    },
}));

const edb = client.getDB();
const ecoll = edb.getCollection(kCollName);

// Send the batched insert as a retryable write (parent session, plain lsid + txnNumber, no
// startTransaction). Server-side FLE2 transform layer will start its own retryable
// internal transaction (lsid with txnUUID) to drive the encrypted writes; the bug lives
// inside that internal transaction.
//
// Caller-supplied stmtIds are deliberately non-contiguous so the bug's "increment from
// base" behavior demonstrably overwrites op2's stmtId.
const parentLsid = {id: UUID()};
const parentTxnNumber = NumberLong(42);
const callerStmtIds = [NumberInt(1), NumberInt(3)];

const insertCmd = {
    insert: kCollName,
    documents: [
        {_id: 0, ssn: "000-00-0001"},
        {_id: 1, ssn: "000-00-0002"},
    ],
    stmtIds: callerStmtIds,
    lsid: parentLsid,
    txnNumber: parentTxnNumber,
};

const insertRes = assert.commandWorked(edb.runCommand(insertCmd));
jsTest.log("Insert reply: " + tojson(insertRes));

// Locate the retryable internal child transaction the FLE2 layer started. It will have
// the same `id` UUID as the parent lsid plus a child `txnUUID`. config.transactions on
// the shard primary records the child session's history.
const shard0Primary = st.rs0.getPrimary();
const configTxns = shard0Primary.getDB("config").getCollection("transactions");
const childTxnDoc = configTxns.findOne({
    "_id.id": parentLsid.id,
    "_id.txnUUID": {$exists: true},
});
assert(childTxnDoc,
       () => "Could not find FLE2-spawned retryable internal child transaction. " +
             "Existing config.transactions docs: " + tojson(configTxns.find().toArray()));
const childLsid = childTxnDoc._id;
const childTxnNumber = Number(childTxnDoc.txnNum);
jsTest.log("Child lsid: " + tojson(childLsid) + " txnNum=" + childTxnNumber);

const oplogEntries = getOplogEntriesForTxn(st.rs0, childLsid, childTxnNumber);
assert.gte(oplogEntries.length, 1, oplogEntries);
const applyOpsEntry = oplogEntries[0];
const ops = applyOpsEntry.o.applyOps;
jsTest.log("applyOps operations: " + tojson(ops));

const clientNamespace = ecoll.getFullName();
const clientOps = ops.filter((op) => op.ns === clientNamespace);
const auxOps = ops.filter((op) => op.ns !== clientNamespace);

assert.eq(clientOps.length, callerStmtIds.length,
          () => "expected one client-doc op per caller stmtId, got " + tojson(clientOps));

// (C1) Caller-supplied stmtIds must be preserved verbatim on the client docs.
const observedClientStmtIds = clientOps.map((op) => op.stmtId).sort((a, b) => a - b);
const expectedClientStmtIds = callerStmtIds.map((s) => Number(s)).sort((a, b) => a - b);
assert.eq(observedClientStmtIds, expectedClientStmtIds,
          () => "C1 violated: client-doc stmtIds drifted. observed=" +
                tojson(observedClientStmtIds) +
                " expected=" + tojson(expectedClientStmtIds) +
                "\nfull ops: " + tojson(ops));

// (C2) Auxiliary ops (ESC, ECOC, __safeContent__ pull-update) must not carry a stmtId
// field -- the applyOps writer strips kUninitializedStmtId (-1).
auxOps.forEach((op) => {
    assert(!op.hasOwnProperty("stmtId"),
           () => "C2 violated: aux op has stmtId. op=" + tojson(op));
});

// (C3) No two ops share a stmtId.
const stmtIdsSeen = ops.filter((op) => op.hasOwnProperty("stmtId")).map((op) => op.stmtId);
const uniqueStmtIds = new Set(stmtIdsSeen);
assert.eq(uniqueStmtIds.size, stmtIdsSeen.length,
          () => "C3 violated: duplicate stmtIds across ops. seen=" + tojson(stmtIdsSeen));

st.stop();
