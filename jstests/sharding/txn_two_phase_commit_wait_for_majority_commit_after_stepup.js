/**
 * Verifies that a node waits for the write done on stepup to become majority committed before
 * resuming coordinating transaction commits.
 *
 * @tags: [uses_transactions, uses_multi_shard_transaction]
 */

// The UUID consistency check uses connections to shards cached on the ShardingTest object, but this
// test causes failovers on a shard, so the cached connection is not usable.
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {stopServerReplication, restartReplSetReplication} from "jstests/libs/write_concern_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {
    runCommitThroughMongosInParallelThread
} from "jstests/sharding/libs/txn_two_phase_commit_util.js";

const dbName = "test";
const collName = "foo";
const ns = dbName + "." + collName;

let st = new ShardingTest({
    shards: 3,
    rs0: {nodes: [{}, {rsConfig: {priority: 0}}]},
    causallyConsistent: true,
    other: {
        mongosOptions: {verbose: 3},
    },
    // By default, our test infrastructure sets the election timeout to a very high value (24
    // hours). For this test, we need a shorter election timeout because it relies on nodes running
    // an election when they do not detect an active primary. Therefore, we are setting the
    // electionTimeoutMillis to its default value.
    initiateWithDefaultElectionTimeout: true
});

let coordinatorReplSetTest = st.rs0;
let participant0 = st.shard0;
let participant1 = st.shard1;
let participant2 = st.shard2;

let lsid = {id: UUID()};
let txnNumber = 0;

const setUp = function() {
    // Create a sharded collection with a chunk on each shard:
    // shard0: [-inf, 0)
    // shard1: [0, 10)
    // shard2: [10, +inf)
    assert.commandWorked(
        st.s.adminCommand({enableSharding: dbName, primaryShard: participant0.shardName}));
    // The default WC is majority and stopServerReplication will prevent satisfying any majority
    // writes.
    assert.commandWorked(st.s.adminCommand(
        {setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}));

    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));
    assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 0}}));
    assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 10}}));
    assert.commandWorked(
        st.s.adminCommand({moveChunk: ns, find: {_id: 0}, to: participant1.shardName}));
    assert.commandWorked(
        st.s.adminCommand({moveChunk: ns, find: {_id: 10}, to: participant2.shardName}));

    // These forced refreshes are not strictly necessary; they just prevent extra TXN log lines
    // from the shards starting, aborting, and restarting the transaction due to needing to
    // refresh after the transaction has started.
    assert.commandWorked(participant0.adminCommand({_flushRoutingTableCacheUpdates: ns}));
    assert.commandWorked(participant1.adminCommand({_flushRoutingTableCacheUpdates: ns}));
    assert.commandWorked(participant2.adminCommand({_flushRoutingTableCacheUpdates: ns}));
    st.refreshCatalogCacheForNs(st.s, ns);

    // Start a new transaction by inserting a document onto each shard.
    assert.commandWorked(st.s.getDB(dbName).runCommand({
        insert: collName,
        documents: [{_id: -5}, {_id: 5}, {_id: 15}],
        lsid: lsid,
        txnNumber: NumberLong(txnNumber),
        stmtId: NumberInt(0),
        startTransaction: true,
        autocommit: false,
    }));
};
setUp();

let coordPrimary = coordinatorReplSetTest.getPrimary();
let coordSecondary = coordinatorReplSetTest.getSecondary();

// Make the commit coordination hang before writing the decision, and send commitTransaction.
let failPoint = configureFailPoint(coordPrimary, "hangBeforeWritingDecision");
let commitThread =
    runCommitThroughMongosInParallelThread(lsid, txnNumber, st.s.host, ErrorCodes.MaxTimeMSExpired);
commitThread.start();
failPoint.wait();

// Stop replication on all nodes in the coordinator replica set so that the write done on stepup
// cannot become majority committed, regardless of which node steps up.
stopServerReplication([coordPrimary, coordSecondary]);

// Induce the coordinator primary to step down, but allow it to immediately step back up.
assert.commandWorked(
    coordPrimary.adminCommand({replSetStepDown: ReplSetTest.kForeverSecs, force: true}));
assert.commandWorked(coordPrimary.adminCommand({replSetFreeze: 0}));

failPoint.off();

// The router should retry commitTransaction against the primary and time out waiting to
// access the coordinator catalog.
commitThread.join();

// Re-enable replication, so that the write done on stepup can become majority committed.
restartReplSetReplication(coordinatorReplSetTest);

// Now, commitTransaction should succeed.
assert.commandWorked(st.s.adminCommand({
    commitTransaction: 1,
    lsid: lsid,
    txnNumber: NumberLong(txnNumber),
    stmtId: NumberInt(0),
    autocommit: false
}));

jsTest.log("Verify that the transaction was committed on all shards.");
assert.eq(3, st.s.getDB(dbName).getCollection(collName).find().itcount());

st.stop();
