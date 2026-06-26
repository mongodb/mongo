/**
 * Test to make sure that transactions doesn't block shard version metadata refresh.
 *
 * @tags: [uses_transactions, uses_multi_shard_transaction]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

let staticMongod = MongoRunner.runMongod({});

let st = new ShardingTest({shards: 2, mongos: 2, other: {rsOptions: {verbose: 1}}});

assert.commandWorked(
    st.s.adminCommand({enableSharding: "test", primaryShard: st.shard0.shardName}),
);
assert.commandWorked(st.s.adminCommand({shardCollection: "test.user", key: {x: 1}}));
assert.commandWorked(st.s.adminCommand({split: "test.user", middle: {x: 42}}));
assert.commandWorked(
    st.s.adminCommand({moveChunk: "test.user", find: {x: 42}, to: st.shard1.shardName}),
);

// Send a normal write to establish the shard versions outside the transaction.
assert.commandWorked(
    st.s.getDB("test").runCommand({
        insert: "user",
        documents: [{x: 41}, {x: 42}],
    }),
);

// Bump the collection version through a different mongos. This leaves st.s with stale routing
// information for shard1 without directly modifying the config server metadata.
assert.commandWorked(st.s1.adminCommand({split: "test.user", middle: {x: 45}}));

let lsid = {id: UUID()};
let txnNumber = 0;

assert.commandWorked(
    st.s1.getDB("test").runCommand({
        insert: "user",
        documents: [{x: 40}, {x: 43}],
        lsid: lsid,
        txnNumber: NumberLong(txnNumber),
        stmtId: NumberInt(0),
        startTransaction: true,
        autocommit: false,
    }),
);

// Make the transaction stay in prepared state so it will hold on to the collection locks.
assert.commandWorked(
    st.rs0
        .getPrimary()
        .getDB("admin")
        .runCommand({configureFailPoint: "hangBeforeWritingDecision", mode: "alwaysOn"}),
);
assert.commandWorked(
    st.rs1
        .getPrimary()
        .getDB("admin")
        .runCommand({configureFailPoint: "hangBeforeWritingDecision", mode: "alwaysOn"}),
);

const runCommitCode =
    "db.adminCommand({" +
    "commitTransaction: 1," +
    "lsid: " +
    tojson(lsid) +
    "," +
    "txnNumber: NumberLong(" +
    txnNumber +
    ")," +
    "stmtId: NumberInt(0)," +
    "autocommit: false," +
    "});";
let commitTxn = startParallelShell(runCommitCode, st.s1.port);

// Insert should be able to refresh the sharding metadata even with existing transactions
// holding the collection lock in IX.
assert.commandWorked(
    st.s.getDB("test").runCommand({insert: "user", documents: [{x: 44}], maxTimeMS: 5 * 1000}),
);

assert.commandWorked(
    st.rs0
        .getPrimary()
        .getDB("admin")
        .runCommand({configureFailPoint: "hangBeforeWritingDecision", mode: "off"}),
);
assert.commandWorked(
    st.rs1
        .getPrimary()
        .getDB("admin")
        .runCommand({configureFailPoint: "hangBeforeWritingDecision", mode: "off"}),
);
commitTxn();

st.stop({skipValidation: true});
MongoRunner.stopMongod(staticMongod);
