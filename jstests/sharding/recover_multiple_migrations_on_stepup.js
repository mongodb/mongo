/**
 * Tests that if on stepup a shard finds more than one config.migrationCoordinators documents (which
 * could happen as a consequence of the bug described in SERVER-62245), the shard will be able to
 * properly recover all migrations.
 *
 * TODO: SERVER-62316 This test should be deleted when 6.0 becomes the lastLTS version, since a
 * situation where there are more than one migrationCoordinators documents will no longer be
 * possible.
 */
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load('./jstests/libs/chunk_manipulation_util.js');

// Disable checking for index consistency to ensure that the config server doesn't trigger a
// StaleShardVersion exception on the shards and cause them to refresh their sharding metadata. That
// would interfere with the precise migration recovery interleaving this test requires.
const nodeOptions = {
    setParameter: {enableShardedIndexConsistencyCheck: false}
};

var st = new ShardingTest({shards: 2, other: {configOptions: nodeOptions, enableBalancer: false}});
let staticMongod = MongoRunner.runMongod({});

const dbName = "test";
const collNameA = "foo";
const collNameB = "bar";
const nsA = dbName + "." + collNameA;
const nsB = dbName + "." + collNameB;
const collA = st.s.getDB(dbName)[collNameA];
const collB = st.s.getDB(dbName)[collNameB];

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
st.ensurePrimaryShard(dbName, st.shard0.shardName);
assert.commandWorked(st.s.adminCommand({shardCollection: nsA, key: {_id: 1}}));
assert.commandWorked(st.s.adminCommand({shardCollection: nsB, key: {_id: 1}}));

// Run a first migration just to save its associated config.migrationCoordinators document for the
// purposes of the test.
var moveChunkHangAtStep3Failpoint = configureFailPoint(st.rs0.getPrimary(), "moveChunkHangAtStep3");
var joinMoveChunk1 = moveChunkParallel(
    staticMongod, st.s0.host, {_id: 0}, null, nsA, st.shard1.shardName, true /* expectSuccess */);
moveChunkHangAtStep3Failpoint.wait();

let migrationCoordinatorDocuments =
    st.rs0.getPrimary().getDB('config')['migrationCoordinators'].find().toArray();
assert.eq(1, migrationCoordinatorDocuments.length);
let firstMigrationCoordinatorDoc = migrationCoordinatorDocuments[0];

// Let the first migration finish and delete its migrationCoordinator document. Otherwise because of
// the fix introduced in SERVER-62296 no new migration could start until the first one has deleted
// its recovery document.
moveChunkHangAtStep3Failpoint.off();
joinMoveChunk1();

// Start a second migration on a different collection, wait until it persists it's recovery document
// and then step down the donor.
var moveChunkHangAtStep3Failpoint = configureFailPoint(st.rs0.getPrimary(), "moveChunkHangAtStep3");
// NOTE: The test doesn't join this parallel migration to avoid the check on its outcome,
// which is not deterministic when executed in a configsvr stepdown suite (SERVER-62419)
moveChunkParallel(staticMongod, st.s0.host, {_id: 0}, null, nsB, st.shard1.shardName);

moveChunkHangAtStep3Failpoint.wait();

// Insert the recovery document from the first migration as to simulate the bug described in
// SERVER-62245
assert.commandWorked(st.rs0.getPrimary().getDB('config')['migrationCoordinators'].insert(
    firstMigrationCoordinatorDoc));

// Now we have two migrations pending to be recovered
assert.eq(2, st.rs0.getPrimary().getDB('config')['migrationCoordinators'].countDocuments({}));

// Stepdown the donor shard
assert.commandWorked(st.rs0.getPrimary().adminCommand({replSetStepDown: 5, force: true}));
moveChunkHangAtStep3Failpoint.off();

// Check that the donor shard has been able to recover the shard version for both collections.
assert.eq(0, collA.find().itcount());
assert.eq(0, collB.find().itcount());

MongoRunner.stopMongod(staticMongod);
st.stop();
})();
