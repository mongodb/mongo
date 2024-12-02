/**
 * Tests behavior of adding participants to existing transactions from a shard.
 *
 * Note that this test currently does not test the full functionality of a shard
 * being able to add transaction participants without going through mongos. Currently,
 * we only test that the transaction requests are valid.
 *
 * @tags: [
 *   requires_sharding,
 *   uses_multi_shard_transaction,
 *   uses_transactions,
 *   requires_fcv_80,
 * ]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

const dbName = "test";
const collName = "foo";
const ns = dbName + "." + collName;

const st = new ShardingTest({shards: 2});

/**
 * Set up a sharded collection with 2 chunks, one on each shard.
 */
jsTest.log("Setting up sharded cluster...");
assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
assert.commandWorked(
    st.s.getDB(dbName)[collName].insert({_id: -1}, {writeConcern: {w: "majority"}}));
assert.commandWorked(
    st.s.getDB(dbName)[collName].insert({_id: 1}, {writeConcern: {w: "majority"}}));

assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));
assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 0}}));
assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {_id: 1}, to: st.shard1.shardName}));
st.refreshCatalogCacheForNs(st.s, ns);

// Refresh second shard to avoid stale shard version error on the second transaction statement.
assert.commandWorked(st.rs1.getPrimary().adminCommand({_flushRoutingTableCacheUpdates: ns}));

/**
 * Test 1: Verifies that the startOrContinueTransaction sessionOption can specify a readConcern.
 *
 * Previously, only the first command in a transaction could specify a readConcern level.
 */
(function verifyStartOrContinueTransactionCanSpecifyReadConcern() {
    jsTest.log("Starting verifyStartOrContinueTransactionCanSpecifyReadConcern...");

    const session = st.shard0.startSession();
    const sessionDB = session.getDatabase(dbName);
    // Start a transaction and target only the first shard.
    session.startTransaction({readConcern: {level: "majority"}});

    assert.commandWorked(sessionDB.runCommand({find: collName, filter: {_id: -1}}));

    // Send another request to the first shard with the startOrContinueTransaction option and
    // the same readConcern and validate that it does not throw an error.
    //
    // Note that the test command targets the shard and not mongos. Clients should never know
    // about this startOrContinueTransaction session option, which should only be used during
    // shard-to-shard communication.
    assert.commandWorked(sessionDB.runCommand({
        find: collName,
        filter: {_id: -1},
        startOrContinueTransaction: true,
        readConcern: {level: "majority"},
    }));

    session.abortTransaction();
    jsTest.log("Exiting verifyStartOrContinueTransactionCanSpecifyReadConcern.");
})();

st.stop();
