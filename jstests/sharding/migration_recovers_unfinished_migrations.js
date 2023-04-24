/**
 * Tests that while there is an unfinished migration pending recovery, if a new migration (of a
 * different collection) attempts to start, it will first need to recover the unfinished migration.
 *
 * @tags: [
 *     # In the event of a config server step down, the new primary balancer may attempt to recover
 *     # that migration by sending a new `moveChunk` command to the donor shard causing the test to
 *     # hang.
 *     does_not_support_stepdowns,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load('jstests/libs/chunk_manipulation_util.js');
load('jstests/replsets/rslib.js');

// Disable checking for index consistency to ensure that the config server doesn't trigger a
// StaleShardVersion exception on the shards and cause them to refresh their sharding metadata. That
// would interfere with the precise migration recovery interleaving this test requires.
const nodeOptions = {
    setParameter: {enableShardedIndexConsistencyCheck: false}
};

// Disable balancer in order to prevent balancing rounds from triggering shard version refreshes on
// the shards that would interfere with the migration recovery interleaving this test requires.
var st = new ShardingTest({
    shards: {rs0: {nodes: 2}, rs1: {nodes: 1}},
    config: 3,
    other: {configOptions: nodeOptions, enableBalancer: false}
});
let staticMongod = MongoRunner.runMongod({});

const dbName = "test";
const collNameA = "foo";
const collNameB = "bar";
const nsA = dbName + "." + collNameA;
const nsB = dbName + "." + collNameB;

assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
assert.commandWorked(st.s.adminCommand({shardCollection: nsA, key: {_id: 1}}));
assert.commandWorked(st.s.adminCommand({shardCollection: nsB, key: {_id: 1}}));

// Hang before commit migration
let moveChunkHangAtStep5Failpoint = configureFailPoint(st.rs0.getPrimary(), "moveChunkHangAtStep5");
var joinMoveChunk1 = moveChunkParallel(
    staticMongod, st.s0.host, {_id: 0}, null, nsA, st.shard1.shardName, true /* expectSuccess */);

moveChunkHangAtStep5Failpoint.wait();

let migrationCommitNetworkErrorFailpoint =
    configureFailPoint(st.rs0.getPrimary(), "migrationCommitNetworkError");
let skipShardFilteringMetadataRefreshFailpoint =
    configureFailPoint(st.rs0.getPrimary(), "skipShardFilteringMetadataRefresh");

moveChunkHangAtStep5Failpoint.off();
migrationCommitNetworkErrorFailpoint.wait();

//  Don't let the migration recovery finish on the secondary that will next be stepped-up.
const rs0Secondary = st.rs0.getSecondary();
let hangInEnsureChunkVersionIsGreaterThanInterruptibleFailpoint =
    configureFailPoint(rs0Secondary, "hangInEnsureChunkVersionIsGreaterThanInterruptible");

st.rs0.stepUp(rs0Secondary);

// Wait for the config server to see the new primary.
// TODO SERVER-74177 Remove this once retry on NotWritablePrimary is implemented.
st.forEachConfigServer((conn) => {
    awaitRSClientHosts(conn, st.rs0.getPrimary(), {ok: true, ismaster: true});
});

joinMoveChunk1();
migrationCommitNetworkErrorFailpoint.off();
skipShardFilteringMetadataRefreshFailpoint.off();

// The migration is left pending recovery.
{
    let migrationCoordinatorDocuments =
        st.rs0.getPrimary().getDB('config')['migrationCoordinators'].find().toArray();
    assert.eq(1, migrationCoordinatorDocuments.length);
    assert.eq(nsA, migrationCoordinatorDocuments[0].nss);
}

// Start a second migration on a different collection and wait until it persists its recovery
// document.
let moveChunkHangAtStep3Failpoint = configureFailPoint(st.rs0.getPrimary(), "moveChunkHangAtStep3");

var joinMoveChunk2 = moveChunkParallel(
    staticMongod, st.s0.host, {_id: 0}, null, nsB, st.shard1.shardName, true /* expectSuccess */);

// Check that second migration won't be able to persist its coordinator document until the shard has
// been able to recover the first migration.
sleep(5 * 1000);
{
    // There's still only one migration recovery document, corresponding to the first migration
    let migrationCoordinatorDocuments =
        st.rs0.getPrimary().getDB('config')['migrationCoordinators'].find().toArray();
    assert.eq(1, migrationCoordinatorDocuments.length);
    assert.eq(nsA, migrationCoordinatorDocuments[0].nss);
}

// Let the migration recovery complete
hangInEnsureChunkVersionIsGreaterThanInterruptibleFailpoint.off();
moveChunkHangAtStep3Failpoint.wait();

// Check that the first migration has been recovered. There must be only one
// config.migrationCoordinators document, which corresponds to the second migration.
{
    let migrationCoordinatorDocuments =
        st.rs0.getPrimary().getDB('config')['migrationCoordinators'].find().toArray();
    assert.eq(1, migrationCoordinatorDocuments.length);
    assert.eq(nsB, migrationCoordinatorDocuments[0].nss);
}

moveChunkHangAtStep3Failpoint.off();
joinMoveChunk2();

MongoRunner.stopMongod(staticMongod);
st.stop();
})();
