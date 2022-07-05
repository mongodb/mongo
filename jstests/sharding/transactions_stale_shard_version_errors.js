// Tests mongos behavior on stale shard version errors received in a transaction.
//
// @tags: [
//  requires_sharding,
//  uses_transactions,
//  uses_multi_shard_transaction,
//  assumes_balancer_off
// ]
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/sharding/libs/sharded_transactions_helpers.js");
load("jstests/multiVersion/libs/verify_versions.js");
load("jstests/sharding/libs/find_chunks_util.js");

function expectChunks(st, ns, chunks) {
    for (let i = 0; i < chunks.length; i++) {
        assert.eq(chunks[i],
                  findChunksUtil.countChunksForNs(
                      st.s.getDB("config"), ns, {shard: st["shard" + i].shardName}),
                  "unexpected number of chunks on shard " + i);
    }
}

const dbName = "test";
const collName = "foo";
const ns = dbName + '.' + collName;

// Disable checking for index consistency to ensure that the config server doesn't trigger a
// StaleShardVersion exception on shards and cause them to refresh their sharding metadata.
const nodeOptions = {
    setParameter: {enableShardedIndexConsistencyCheck: false}
};

const st = new ShardingTest(
    {shards: 3, mongos: 2, other: {configOptions: nodeOptions, enableBalancer: false}});

enableStaleVersionAndSnapshotRetriesWithinTransactions(st);

// Disable the best-effort recipient metadata refresh after migrations to simplify simulating
// stale shard version errors.
assert.commandWorked(st.rs0.getPrimary().adminCommand(
    {configureFailPoint: "migrationRecipientFailPostCommitRefresh", mode: "alwaysOn"}));
assert.commandWorked(st.rs1.getPrimary().adminCommand(
    {configureFailPoint: "migrationRecipientFailPostCommitRefresh", mode: "alwaysOn"}));
assert.commandWorked(st.rs2.getPrimary().adminCommand(
    {configureFailPoint: "migrationRecipientFailPostCommitRefresh", mode: "alwaysOn"}));

// Shard two collections in the same database, each with 2 chunks, [minKey, 0), [0, maxKey),
// with one document each, all on Shard0.

assert.commandWorked(
    st.s.getDB(dbName)[collName].insert({_id: -5}, {writeConcern: {w: "majority"}}));
assert.commandWorked(
    st.s.getDB(dbName)[collName].insert({_id: 5}, {writeConcern: {w: "majority"}}));

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
st.ensurePrimaryShard(dbName, st.shard0.shardName);

assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));
assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 0}}));

expectChunks(st, ns, [2, 0, 0]);

const otherCollName = "bar";
const otherNs = dbName + "." + otherCollName;

assert.commandWorked(
    st.s.getDB(dbName)[otherCollName].insert({_id: -5}, {writeConcern: {w: "majority"}}));
assert.commandWorked(
    st.s.getDB(dbName)[otherCollName].insert({_id: 5}, {writeConcern: {w: "majority"}}));

assert.commandWorked(st.s.adminCommand({shardCollection: otherNs, key: {_id: 1}}));
assert.commandWorked(st.s.adminCommand({split: otherNs, middle: {_id: 0}}));

expectChunks(st, otherNs, [2, 0, 0]);

const session = st.s.startSession();
const sessionDB = session.getDatabase(dbName);

//
// Stale shard version on first overall command should succeed.
//

// Move a chunk in the first collection from Shard0 to Shard1 through the main mongos, so Shard1
// is stale but not the router.
assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {_id: 5}, to: st.shard1.shardName}));
expectChunks(st, ns, [1, 1, 0]);

session.startTransaction();

// Targets Shard1, which is stale.
assert.commandWorked(sessionDB.runCommand({find: collName, filter: {_id: 5}}));

assert.commandWorked(session.commitTransaction_forTesting());

//
// Stale shard version on second command to a shard should fail.
//

expectChunks(st, ns, [1, 1, 0]);

// Move a chunk in the other collection from Shard0 to Shard1 through the main mongos, so Shard1
// is stale for the other collection but not the router.
assert.commandWorked(
    st.s.adminCommand({moveChunk: otherNs, find: {_id: 5}, to: st.shard1.shardName}));
expectChunks(st, otherNs, [1, 1, 0]);

session.startTransaction();

// Targets Shard1 for the first ns, which is not stale.
assert.commandWorked(sessionDB.runCommand({find: collName, filter: {_id: 5}}));

// Targets the other sharded collection on Shard1, which is stale. Because a previous statement
// has executed on Shard1, the retry will not restart the transaction, and will fail when it
// finds the transaction has aborted because of the stale shard version.
let res = assert.commandFailedWithCode(
    sessionDB.runCommand({find: otherCollName, filter: {_id: 5}}), ErrorCodes.NoSuchTransaction);
assert.eq(res.errorLabels, ["TransientTransactionError"]);

assertNoSuchTransactionOnAllShards(st, session.getSessionId(), session.getTxnNumber_forTesting());
assert.commandFailedWithCode(session.abortTransaction_forTesting(), ErrorCodes.NoSuchTransaction);

//
// Stale shard version on first command to a new shard should succeed.
//

expectChunks(st, ns, [1, 1, 0]);

// Move a chunk for the other collection from Shard1 to Shard0 through the main mongos, so
// Shard0 is stale for it and the router is not.
assert.commandWorked(
    st.s.adminCommand({moveChunk: otherNs, find: {_id: 5}, to: st.shard0.shardName}));
expectChunks(st, otherNs, [2, 0, 0]);

session.startTransaction();

// Targets Shard1 for the first ns, which is not stale.
assert.commandWorked(sessionDB.runCommand({find: collName, filter: {_id: 5}}));

// Targets Shard0 for the other ns, which is stale.
assert.commandWorked(sessionDB.runCommand({find: otherCollName, filter: {_id: 5}}));

assert.commandWorked(session.commitTransaction_forTesting());

//
// Stale mongos aborts on old shard.
//

// Move a chunk in the first collection from Shard1 to Shard0 through the other mongos, so
// Shard1 and the main mongos are stale for it.
const otherMongos = st.s1;
assert.commandWorked(
    otherMongos.adminCommand({moveChunk: ns, find: {_id: 5}, to: st.shard0.shardName}));
expectChunks(st, ns, [2, 0, 0]);

session.startTransaction();

// Targets Shard1, which hits a stale version error, then re-targets Shard0, which is also
// stale but should succeed.
assert.commandWorked(sessionDB.runCommand({find: collName, filter: {_id: 5}}));

assert.commandWorked(session.commitTransaction_forTesting());

// Verify there is no in-progress transaction on Shard1.
res = assert.commandFailedWithCode(st.rs1.getPrimary().getDB(dbName).runCommand({
    find: collName,
    lsid: session.getSessionId(),
    txnNumber: NumberLong(session.getTxnNumber_forTesting()),
    autocommit: false,
}),
                                   ErrorCodes.NoSuchTransaction);
assert.eq(res.errorLabels, ["TransientTransactionError"]);

//
// More than one stale shard version error.
//

// Move chunks for the first ns from Shard0 to Shard1 and Shard2 through the main mongos, so
// both are stale but not the router.
assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {_id: 5}, to: st.shard2.shardName}));
expectChunks(st, ns, [1, 0, 1]);

assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {_id: -5}, to: st.shard1.shardName}));
expectChunks(st, ns, [0, 1, 1]);

session.startTransaction();

// Targets all shards, two of which are stale.
assert.commandWorked(sessionDB.runCommand({find: collName}));

assert.commandWorked(session.commitTransaction_forTesting());

//
// Can retry a stale write on the first statement.
//

// Move a chunk to Shard1 to make it stale.
assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {_id: 5}, to: st.shard1.shardName}));
expectChunks(st, ns, [0, 2, 0]);

session.startTransaction();

// Targets Shard1, which is stale.
assert.commandWorked(sessionDB.runCommand({insert: collName, documents: [{_id: 6}]}));

assert.commandWorked(session.commitTransaction_forTesting());

//
// Can retry a stale write past the first statement if the write has been sent to only new
// participant shard(s).
//
// TODO SERVER-37207: Change batch writes to retry only the failed writes in a batch, to allow
// retrying writes beyond the first overall statement.
//

// Move a chunk to Shard2 to make it stale.
assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {_id: 5}, to: st.shard2.shardName}));
expectChunks(st, ns, [0, 1, 1]);

session.startTransaction();

// Targets Shard1, which is not stale.
assert.commandWorked(sessionDB.runCommand({insert: collName, documents: [{_id: -4}]}));

// Targets Shard2, which is stale.
assert.commandWorked(sessionDB.runCommand({insert: collName, documents: [{_id: 7}]}));

assert.commandWorked(session.commitTransaction_forTesting());

//
// The final StaleConfig error should be returned if the router exhausts its retries.
//

// Move a chunk to Shard0 to make it stale.
assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {_id: -5}, to: st.shard0.shardName}));
expectChunks(st, ns, [1, 0, 1]);

// Disable metadata refreshes on the stale shard so it will indefinitely return a stale version
// error.
assert.commandWorked(st.rs0.getPrimary().adminCommand(
    {configureFailPoint: "skipShardFilteringMetadataRefresh", mode: "alwaysOn"}));

session.startTransaction();

// Target Shard2, to verify the transaction on it is aborted implicitly later.
assert.commandWorked(sessionDB.runCommand({find: collName, filter: {_id: 5}}));

// Targets all shards. Shard0 is stale and won't refresh its metadata, so mongos should exhaust
// its retries and implicitly abort the transaction.
res = assert.commandFailedWithCode(sessionDB.runCommand({find: collName}), ErrorCodes.StaleConfig);
assert.eq(res.errorLabels, ["TransientTransactionError"]);

// Verify the shards that did not return an error were also aborted.
assertNoSuchTransactionOnAllShards(st, session.getSessionId(), session.getTxnNumber_forTesting());
assert.commandFailedWithCode(session.abortTransaction_forTesting(), ErrorCodes.NoSuchTransaction);

assert.commandWorked(st.rs0.getPrimary().adminCommand(
    {configureFailPoint: "skipShardFilteringMetadataRefresh", mode: "off"}));

disableStaleVersionAndSnapshotRetriesWithinTransactions(st);

st.stop();
})();
