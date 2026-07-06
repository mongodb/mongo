/**
 * Positive companion to start_or_continue_participant_failover.js: a legitimate brand-new
 * sub-router participant (first contact via $lookup, no prior incarnation on this transaction)
 * must still join normally.
 *
 * The router records the new participant's replication term on first observation and does not
 * abort; only a later term mismatch (an actual failover) aborts the transaction. Verifies the
 * transaction commits and that the mongos logged the first-observation recording (id 12812800).
 *
 * @tags: [
 *   requires_sharding,
 *   uses_transactions,
 *   uses_multi_shard_transaction
 * ]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";
import {checkLog} from "src/mongo/shell/check_log.js";

const dbName = "test";
const localColl = "local_coll";
const foreignColl = "foreign_coll";
const foreignNs = dbName + "." + foreignColl;

const st = new ShardingTest({shards: 2, rs: {nodes: 3}});

// The first-observation recording is logged at debug level 3 in the transaction component
// (id 12812800, transaction_router.cpp). Raise the mongos verbosity so checkLog can see it.
assert.commandWorked(
    st.s.adminCommand({setParameter: 1, logComponentVerbosity: {transaction: {verbosity: 3}}}),
);

assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}),
);

// Shard `foreign` and place all chunks on shard1, so the $lookup forces shard0 to fan out
// to shard1.
assert.commandWorked(st.s.adminCommand({shardCollection: foreignNs, key: {_id: 1}}));
assert.commandWorked(
    st.s.adminCommand({moveChunk: foreignNs, find: {_id: MinKey}, to: st.shard1.shardName}),
);
st.refreshCatalogCacheForNs(st.s, foreignNs);
assert.commandWorked(st.rs0.getPrimary().adminCommand({_flushRoutingTableCacheUpdates: foreignNs}));
assert.commandWorked(st.rs1.getPrimary().adminCommand({_flushRoutingTableCacheUpdates: foreignNs}));

// Pre-populate `local` (unsharded, on shard0) so the aggregate has input.
assert.commandWorked(
    st.s.getDB(dbName)[localColl].insert({_id: 1, key: "K"}, {writeConcern: {w: "majority"}}),
);
// Pre-populate `foreign` (sharded, on shard1) so the $lookup actually returns something.
assert.commandWorked(
    st.s
        .getDB(dbName)
        [foreignColl].insert({_id: "K", value: "joined"}, {writeConcern: {w: "majority"}}),
);

// Pre-warm shard0's catalog cache for `foreign_coll` outside the txn.
st.s
    .getDB(dbName)
    [localColl].aggregate([
        {$lookup: {from: foreignColl, localField: "key", foreignField: "_id", as: "joined"}},
    ])
    .toArray();

const lsid = {id: UUID()};
const txnNumber = NumberLong(1);
const mongosDB = st.s.getDB(dbName);

jsTest.log(
    "Step 1: insert {_id: 'X'} into local (shard0); mongos has shard0 in its participant map but has not contacted shard1.",
);
assert.commandWorked(
    mongosDB.runCommand({
        insert: localColl,
        documents: [{_id: "X", key: "K"}],
        lsid: lsid,
        txnNumber: txnNumber,
        stmtId: NumberInt(0),
        startTransaction: true,
        autocommit: false,
    }),
);

jsTest.log(
    "Step 2: aggregate with $lookup: shard0 sub-routes to shard1 (a brand-new participant); mongos records its term on first observation.",
);
const aggRes = assert.commandWorked(
    mongosDB.runCommand({
        aggregate: localColl,
        pipeline: [
            {$lookup: {from: foreignColl, localField: "key", foreignField: "_id", as: "joined"}},
        ],
        cursor: {},
        lsid: lsid,
        txnNumber: txnNumber,
        stmtId: NumberInt(1),
        autocommit: false,
    }),
);
jsTest.log.info("aggregate succeeded", {firstBatch: aggRes.cursor.firstBatch});
// local holds the pre-populated {_id:1} plus the in-txn {_id:"X"} insert, so the snapshot-read
// aggregate returns both, each with the foreign document joined in.
assert.eq(2, aggRes.cursor.firstBatch.length, "expected two joined documents");
for (const doc of aggRes.cursor.firstBatch) {
    assert.eq("K", doc.key);
    assert.eq(1, doc.joined.length, "expected the $lookup to find a match on shard1");
}

jsTest.log(
    "Step 3: commit must succeed: no participant primary changed, so the brand-new sub-router join is legitimate.",
);
const commitRes = assert.commandWorked(
    st.s.adminCommand({
        commitTransaction: 1,
        lsid: lsid,
        txnNumber: txnNumber,
        autocommit: false,
    }),
);
jsTest.log.info("commit response", {commitRes});

jsTest.log("Step 4: verify both inserts persisted.");
const localDocs = st.s.getDB(dbName)[localColl].find({_id: "X"}).toArray();
assert.eq(1, localDocs.length, "X must be present in local on shard0 after commit");
const foreignDocs = st.s.getDB(dbName)[foreignColl].find({_id: "K"}).toArray();
assert.eq(1, foreignDocs.length, "K must remain in foreign on shard1 after commit");

jsTest.log(
    "Step 5: verify the mongos actually recorded shard1's term on first observation (log id 12812800).",
);
checkLog.containsJson(st.s, 12812800, {shardId: st.shard1.shardName});

jsTest.log(
    "Step 6: drive a second transaction; the participant primaries are unchanged, so it also commits.",
);
const lsid2 = {id: UUID()};
const txnNumber2 = NumberLong(1);
assert.commandWorked(
    mongosDB.runCommand({
        insert: localColl,
        documents: [{_id: "X2", key: "K"}],
        lsid: lsid2,
        txnNumber: txnNumber2,
        stmtId: NumberInt(0),
        startTransaction: true,
        autocommit: false,
    }),
);
assert.commandWorked(
    mongosDB.runCommand({
        aggregate: localColl,
        pipeline: [
            {$lookup: {from: foreignColl, localField: "key", foreignField: "_id", as: "joined"}},
        ],
        cursor: {},
        lsid: lsid2,
        txnNumber: txnNumber2,
        stmtId: NumberInt(1),
        autocommit: false,
    }),
);
assert.commandWorked(
    st.s.adminCommand({
        commitTransaction: 1,
        lsid: lsid2,
        txnNumber: txnNumber2,
        autocommit: false,
    }),
);

const localDocs2 = st.s.getDB(dbName)[localColl].find({_id: "X2"}).toArray();
assert.eq(1, localDocs2.length, "X2 must be present in local on shard0 after the 2nd commit");

jsTest.log("Confirmed: a brand-new sub-router participant joins normally and commits.");

st.stop();
