/**
 * Runs a single-write-shard transaction which commits, but for which the client retries commit and
 * a read-only shard fails over before the second commit attempt.
 *
 * The second commit attempt should still return a commit decision.
 *
 * This test was written to reproduce the bug in the original single-write-shard transaction commit
 * optimization where if a read-only shard failed over before the client sent a second commit
 * attempt, the second commit attempt would return NoSuchTransaction with TransientTransactionError,
 * causing the client to retry the whole transaction at a higher transaction number and the
 * transaction's write to be applied twice.
 *
 * @tags: [
 *   uses_multi_shard_transaction,
 *   uses_transactions,
 * ]
 */

(function() {
'use strict';

load("jstests/libs/fail_point_util.js");

const db1Name = "db1";
const coll1Name = "foo";
const ns1 = db1Name + "." + coll1Name;

const db2Name = "db2";
const coll2Name = "bar";
const ns2 = db2Name + "." + coll2Name;

const st = new ShardingTest({
    shards: {rs0: {nodes: 2}, rs1: {nodes: 1}},
    config: TestData.configShard ? undefined : 1,
    other: {
        mongosOptions: {verbose: 3},
    }
});

jsTest.log("Create two databases on different primary shards.");
// enableSharding creates the databases.
assert.commandWorked(st.s.adminCommand({enableSharding: db1Name}));
assert.commandWorked(st.s.adminCommand({enableSharding: db2Name}));
assert.commandWorked(st.s.adminCommand({movePrimary: db1Name, to: st.shard0.shardName}));
assert.commandWorked(st.s.adminCommand({movePrimary: db2Name, to: st.shard1.shardName}));

jsTest.log("Insert data on both shards.");
// This ensures all nodes refresh their routing caches.
st.s.getDB(db1Name).getCollection(coll1Name).insert({_id: "dummy"});
st.s.getDB(db2Name).getCollection(coll2Name).insert({_id: "dummy"});

jsTest.log("Run a single-write-shard transaction and commit it.");
const session = st.s.startSession();
session.startTransaction();
session.getDatabase(db1Name).getCollection(coll1Name).findOne({_id: "readOperationOnShard0"});
session.getDatabase(db2Name).getCollection(coll2Name).insert({_id: "writeOperationOnShard1"});
// Use adminCommand so we can pass writeConcern.
assert.commandWorked(st.s.adminCommand({
    commitTransaction: 1,
    lsid: session.getSessionId(),
    txnNumber: session.getTxnNumber_forTesting(),
    autocommit: false,
    writeConcern: {w: "majority"},
}));

jsTest.log("Induce a failover on the read shard.");
assert.commandWorked(st.rs0.getPrimary().adminCommand({replSetStepDown: 60, force: true}));

jsTest.log("Make second attempt to commit, should still return that the transaction committed");
assert.commandWorked(session.commitTransaction_forTesting());

st.stop();
})();
