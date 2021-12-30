/**
 * Tests that while there is an unfinished migration pending recovery, if a new migration (of a
 * different collection) attempts to start, it will first need to recover the unfinished migration.
 */
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load('jstests/libs/chunk_manipulation_util.js');

// Disable checking for index consistency to ensure that the config server doesn't trigger a
// StaleShardVersion exception on the shards and cause them to refresh their sharding metadata. That
// would interfere with the precise migration recovery interleaving this test requires.
const nodeOptions = {
    setParameter: {enableShardedIndexConsistencyCheck: false}
};

// Disable balancer in order to prevent balancing rounds from triggering shard version refreshes on
// the shards that would interfere with the migration recovery interleaving this test requires.
var st = new ShardingTest({shards: 2, other: {configOptions: nodeOptions, enableBalancer: false}});
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
