//
// Tests during chunk migration that the recipient does not receive out of range deletes from
// the donor.
//
// Pauses the migration on the recipient shard after the initial data chunk cloning is finished.
// This allows time for the donor shard to perform deletes, half of which are on the migrating
// chunk. The recipient is then set to continue, collecting the delete mods from the donor, and
// finishes the migration. A failpoint is set prior to resuming in the recipient shard to fail
// if it receives an out of chunk range delete from the donor's delete mods log.
//
// The idea is that the recipient shard should not be collecting deletes from the donor shard
// that are not in range and that will unnecessarily prevent the migration from finishing: the
// migration can only end when donor's log of delete mods for the migrating chunk is empty.
//

load('./jstests/libs/chunk_manipulation_util.js');

(function() {
"use strict";

var staticMongod = MongoRunner.runMongod({});  // For startParallelOps.

/**
 * Start up new sharded cluster, stop balancer that would interfere in manual chunk management.
 */

var st = new ShardingTest({ shards : 2, mongos : 1 });
st.stopBalancer();

var mongos = st.s0,
    admin = mongos.getDB('admin'),
    shards = mongos.getCollection('config.shards').find().toArray(),
    dbName = "testDB",
    ns = dbName + ".foo",
    coll = mongos.getCollection(ns),
    donor = st.shard0,
    recipient = st.shard1,
    donorColl = donor.getCollection(ns),
    recipientColl = recipient.getCollection(ns);

/**
 * Exable sharding, and split collection into two chunks.
 */

// Two chunks
// Donor:     [0, 10) [10, 20)
// Recipient:
jsTest.log('Enable sharding of the collection and pre-split into two chunks....');
assert.commandWorked( admin.runCommand( {enableSharding: dbName} ) );
st.ensurePrimaryShard(dbName, shards[0]._id);
assert.commandWorked( admin.runCommand( {shardCollection: ns, key: {a: 1}} ) );
assert.commandWorked( admin.runCommand( {split: ns, middle: {a: 10}} ) );

/**
 * Insert data into collection
 */

// 10 documents in each chunk on the donor
jsTest.log('Inserting 20 docs into donor shard, 10 in each chunk....');
for (var i = 0; i < 20; ++i) donorColl.insert( {a: i} );
assert.eq(null, donorColl.getDB().getLastError());
assert.eq(20, donorColl.count());

/**
 * Set failpoints. Recipient will crash if an out of chunk range data delete is
 * received from donor. Recipient will pause migration after cloning chunk data from donor,
 * before checking delete mods log on donor.
 */

jsTest.log('setting failpoint failMigrationReceivedOutOfRangeDelete');
assert.commandWorked(recipient.getDB('admin').runCommand( {configureFailPoint: 'failMigrationReceivedOutOfRangeDelete', mode: 'alwaysOn'} ))

jsTest.log('setting recipient failpoint cloned');
pauseMigrateAtStep(recipient, migrateStepNames.cloned);

/**
 * Start a moveChunk in the background. Move chunk [10, 20), which has 10 docs,
 * from shard 0 (donor) to shard 1 (recipient). Migration will pause after cloning
 * (when it reaches the recipient failpoint).
 */

// Donor:     [0, 10)
// Recipient:    [10, 20)
jsTest.log('starting migration, pause after cloning...');
var joinMoveChunk = moveChunkParallel(
    staticMongod,
    st.s0.host,
    {a: 10},
    null,
    coll.getFullName(),
    shards[1]._id);

/**
 * Wait for recipient to finish cloning.
 * THEN delete 10 documents on donor, 5 in the migrating chunk and the 5 in the remaining chunk.
 */

jsTest.log('Delete 5 docs from each chunk, migrating chunk and remaining chunk...');
waitForMigrateStep(recipient, migrateStepNames.cloned);
donorColl.remove( {$and : [ {a: {$gte: 5}}, {a: {$lt: 15}} ]} );

/**
 * Finish migration. Unpause recipient migration, wait for it to collect
 * the delete diffs from donor and finish.
 */

jsTest.log('Continuing and finishing migration...');
unpauseMigrateAtStep(recipient, migrateStepNames.cloned);
joinMoveChunk();

/**
 * Check documents are where they should be: 5 docs in each shard's chunk.
 */

jsTest.log('Checking that documents are on the shards they should be...');
assert.eq(5, donorColl.count());
assert.eq(5, recipientColl.count());
assert.eq(10, coll.count());

jsTest.log('DONE!');
st.stop();

})()
