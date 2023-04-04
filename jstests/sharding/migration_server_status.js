/*
 * Tests that serverStatus includes a migration status when called on the source shard of an active
 * migration.
 *
 * @tags: [requires_fcv_63, temporary_catalog_shard_incompatible]
 */

load('./jstests/libs/chunk_manipulation_util.js');
load("jstests/libs/feature_flag_util.js");

(function() {
'use strict';

var staticMongod = MongoRunner.runMongod({});  // For startParallelOps.

var st = new ShardingTest({shards: 2, mongos: 1});

var mongos = st.s0;
var admin = mongos.getDB("admin");
var coll = mongos.getCollection("migration_server_status.coll");
const usingSetClusterParameter =
    FeatureFlagUtil.isPresentAndEnabled(st.config, "ClusterCardinalityParameter");

assert.commandWorked(
    admin.runCommand({enableSharding: coll.getDB() + "", primaryShard: st.shard0.shardName}));
assert.commandWorked(admin.runCommand({shardCollection: coll + "", key: {_id: 1}}));
assert.commandWorked(admin.runCommand({split: coll + "", middle: {_id: 0}}));

// Mimic inserts for a retryable-write session
var documents = [];
for (var x = -2600; x < 2400; x++) {
    documents.push({_id: x});
}
assert.commandWorked(
    mongos.getDB("migration_server_status")
        .runCommand(
            {insert: "coll", documents: documents, lsid: {id: UUID()}, txnNumber: NumberLong(1)}));

// Pause the migration once it starts on both shards -- somewhat arbitrary pause point.
pauseMoveChunkAtStep(st.shard0, moveChunkStepNames.startedMoveChunk);

var joinMoveChunk = moveChunkParallel(
    staticMongod, st.s0.host, {_id: 1}, null, coll.getFullName(), st.shard1.shardName);

var assertMigrationStatusOnServerStatus = function(serverStatusResult,
                                                   sourceShard,
                                                   destinationShard,
                                                   isDonorShard,
                                                   minKey,
                                                   maxKey,
                                                   collectionName) {
    var migrationResult = serverStatusResult.sharding.migrations;
    assert.eq(sourceShard, migrationResult.source);
    assert.eq(destinationShard, migrationResult.destination);
    assert.eq(isDonorShard, migrationResult.isDonorShard);
    assert.eq(minKey, migrationResult.chunk.min);
    assert.eq(maxKey, migrationResult.chunk.max);
    assert.eq(collectionName, migrationResult.collection);
};

var assertSessionMigrationStatusSource = function(
    serverStatusResult, expectedEntriesToBeMigrated, expectedEntriesSkippedLowerBound) {
    var migrationResult = serverStatusResult.sharding.migrations;

    // If the expected value is null, just check that the field exists
    if (expectedEntriesToBeMigrated == null) {
        assert(migrationResult.sessionOplogEntriesToBeMigratedSoFar);
    } else {
        assert.eq(migrationResult.sessionOplogEntriesToBeMigratedSoFar,
                  expectedEntriesToBeMigrated);
    }

    // If the expected value is null, just check that the field exists
    if (expectedEntriesSkippedLowerBound == null) {
        assert(migrationResult.sessionOplogEntriesSkippedSoFarLowerBound);
    } else {
        // Running DDL operations increases this number by 1
        let actualEntriesSkippedLowerBound = usingSetClusterParameter
            ? expectedEntriesSkippedLowerBound + 1
            : expectedEntriesSkippedLowerBound;
        assert.eq(migrationResult.sessionOplogEntriesSkippedSoFarLowerBound,
                  actualEntriesSkippedLowerBound);
    }
};

var assertSessionMigrationStatusDestination = function(serverStatusResult,
                                                       expectedEntriesMigrated) {
    var migrationResult = serverStatusResult.sharding.migrations;

    // If the expected value is null, just check that the field exists
    if (expectedEntriesMigrated == null) {
        assert(migrationResult.sessionOplogEntriesMigrated);
    } else {
        assert.eq(migrationResult.sessionOplogEntriesMigrated, expectedEntriesMigrated);
    }
};

waitForMoveChunkStep(st.shard0, moveChunkStepNames.startedMoveChunk);

// Source shard should return a migration status.
var shard0ServerStatus = st.shard0.getDB('admin').runCommand({serverStatus: 1});
assert(shard0ServerStatus.sharding.migrations);
assertMigrationStatusOnServerStatus(shard0ServerStatus,
                                    st.shard0.shardName,
                                    st.shard1.shardName,
                                    true,
                                    {"_id": 0},
                                    {"_id": {"$maxKey": 1}},
                                    coll + "");
assertSessionMigrationStatusSource(shard0ServerStatus, null, null);

// Destination shard should return a migration status.
var shard1ServerStatus = st.shard1.getDB('admin').runCommand({serverStatus: 1});
assert(shard1ServerStatus.sharding.migrations);
assertMigrationStatusOnServerStatus(shard1ServerStatus,
                                    st.shard0.shardName,
                                    st.shard1.shardName,
                                    false,
                                    {"_id": 0},
                                    {"_id": {"$maxKey": 1}},
                                    coll + "");
assertSessionMigrationStatusDestination(shard1ServerStatus, null);

// Mongos should never return a migration status.
var mongosServerStatus = st.s0.getDB('admin').runCommand({serverStatus: 1});
assert(!mongosServerStatus.sharding.migrations);

// Pause the migration once chunk data is comitted. At this point we know that the sessions
// are fully transferred because chunk migration only happens after session migration is complete.
pauseMoveChunkAtStep(st.shard0, moveChunkStepNames.chunkDataCommitted);
unpauseMoveChunkAtStep(st.shard0, moveChunkStepNames.startedMoveChunk);
waitForMoveChunkStep(st.shard0, moveChunkStepNames.chunkDataCommitted);

// Source shard should have the correct server status
shard0ServerStatus = st.shard0.getDB('admin').runCommand({serverStatus: 1});
assert(shard0ServerStatus.sharding.migrations);
assertMigrationStatusOnServerStatus(shard0ServerStatus,
                                    st.shard0.shardName,
                                    st.shard1.shardName,
                                    true,
                                    {"_id": 0},
                                    {"_id": {"$maxKey": 1}},
                                    coll + "");
assertSessionMigrationStatusSource(shard0ServerStatus, 2400, 2600);

// Destination shard should have the correct server status
shard1ServerStatus = st.shard1.getDB('admin').runCommand({serverStatus: 1});
assert(shard1ServerStatus.sharding.migrations);
assertMigrationStatusOnServerStatus(shard1ServerStatus,
                                    st.shard0.shardName,
                                    st.shard1.shardName,
                                    false,
                                    {"_id": 0},
                                    {"_id": {"$maxKey": 1}},
                                    coll + "");
assertSessionMigrationStatusDestination(shard1ServerStatus, 2400);

unpauseMoveChunkAtStep(st.shard0, moveChunkStepNames.chunkDataCommitted);

joinMoveChunk();

// Migration is over, should no longer get a migration status.
shard0ServerStatus = st.shard0.getDB('admin').runCommand({serverStatus: 1});
assert(!shard0ServerStatus.sharding.migrations);
var shard1ServerStatus = st.shard0.getDB('admin').runCommand({serverStatus: 1});
assert(!shard1ServerStatus.sharding.migrations);

st.stop();
MongoRunner.stopMongod(staticMongod);
})();
