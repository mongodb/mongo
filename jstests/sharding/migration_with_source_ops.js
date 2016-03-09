//
// Tests during chunk migration that the recipient does not receive out of range operations from
// the donor.
//
// Pauses the migration on the recipient shard after the initial data chunk cloning is finished.
// This allows time for the donor shard to perform inserts/deletes/updates, half of which are on
// the migrating chunk. The recipient is then set to continue, collecting the mods from the
// donor's transfer mods log, and finishes the migration. A failpoint is set prior to resuming
// in the recipient shard to fail if it receives an out of chunk range insert/delete/update from
// the donor's transfer mods log.
//
// The idea is that the recipient shard should not be collecting inserts/deletes/updates from
// the donor shard's transfer mods log that are not in range and will unnecessarily prevent the
// migration from finishing: the migration can only end when donor's log of mods for the migrating
// chunk is empty.
//

load('./jstests/libs/chunk_manipulation_util.js');

(function() {
    "use strict";

    var staticMongod = MongoRunner.runMongod({});  // For startParallelOps.

    /**
     * Start up new sharded cluster, stop balancer that would interfere in manual chunk management.
     */

    var st = new ShardingTest({shards: 2, mongos: 1});
    st.stopBalancer();

    var mongos = st.s0, admin = mongos.getDB('admin'),
        shards = mongos.getCollection('config.shards').find().toArray(), dbName = "testDB",
        ns = dbName + ".foo", coll = mongos.getCollection(ns), donor = st.shard0,
        recipient = st.shard1, donorColl = donor.getCollection(ns),
        recipientColl = recipient.getCollection(ns);

    /**
     * Exable sharding, and split collection into two chunks.
     */

    // Two chunks
    // Donor:     [0, 20) [20, 40)
    // Recipient:
    jsTest.log('Enabling sharding of the collection and pre-splitting into two chunks....');
    assert.commandWorked(admin.runCommand({enableSharding: dbName}));
    st.ensurePrimaryShard(dbName, shards[0]._id);
    assert.commandWorked(admin.runCommand({shardCollection: ns, key: {a: 1}}));
    assert.commandWorked(admin.runCommand({split: ns, middle: {a: 20}}));

    /**
     * Insert data into collection
     */

    // 10 documents in each chunk on the donor
    jsTest.log('Inserting 20 docs into donor shard, 10 in each chunk....');
    for (var i = 0; i < 10; ++i)
        assert.writeOK(coll.insert({a: i}));
    for (var i = 20; i < 30; ++i)
        assert.writeOK(coll.insert({a: i}));
    assert.eq(20, coll.count());

    /**
     * Set failpoints. Recipient will crash if an out of chunk range data op is
     * received from donor. Recipient will pause migration after cloning chunk data from donor,
     * before checking transfer mods log on donor.
     */

    jsTest.log('Setting failpoint failMigrationReceivedOutOfRangeOperation');
    assert.commandWorked(recipient.getDB('admin').runCommand(
        {configureFailPoint: 'failMigrationReceivedOutOfRangeOperation', mode: 'alwaysOn'}));

    jsTest.log(
        'Setting chunk migration recipient failpoint so that it pauses after bulk clone step');
    pauseMigrateAtStep(recipient, migrateStepNames.cloned);

    /**
     * Start a moveChunk in the background. Move chunk [20, 40), which has 10 docs in the
     * range, from shard 0 (donor) to shard 1 (recipient). Migration will pause after
     * cloning step (when it reaches the recipient failpoint).
     */

    // Donor:     [0, 20)
    // Recipient:    [20, 40)
    jsTest.log('Starting migration, pause after cloning...');
    var joinMoveChunk = moveChunkParallel(
        staticMongod, st.s0.host, {a: 20}, null, coll.getFullName(), shards[1]._id);

    /**
     * Wait for recipient to finish cloning step.
     * THEN delete 10 documents on the donor shard, 5 in the migrating chunk and 5 in the remaining
     *chunk.
     * AND insert 2 documents on the donor shard, 1 in the migrating chunk and 1 in the remaining
     *chunk.
     * AND update 2 documents on the donor shard, 1 in the migrating chunk and 1 in the remaining
     *chunk.
     *
     * This will populate the migration transfer mods log, which the recipient will collect when it
     * is unpaused.
     */

    waitForMigrateStep(recipient, migrateStepNames.cloned);

    jsTest.log('Deleting 5 docs from each chunk, migrating chunk and remaining chunk...');
    assert.writeOK(coll.remove({$and: [{a: {$gte: 5}}, {a: {$lt: 25}}]}));

    jsTest.log('Inserting 1 in the migrating chunk range and 1 in the remaining chunk range...');
    assert.writeOK(coll.insert({a: 10}));
    assert.writeOK(coll.insert({a: 30}));

    jsTest.log('Updating 1 in the migrating chunk range and 1 in the remaining chunk range...');
    assert.writeOK(coll.update({a: 0}, {a: 0, updatedData: "updated"}));
    assert.writeOK(coll.update({a: 25}, {a: 25, updatedData: "updated"}));

    /**
     * Finish migration. Unpause recipient migration, wait for it to collect
     * the new ops from the donor shard's migration transfer mods log, and finish.
     */

    jsTest.log('Continuing and finishing migration...');
    unpauseMigrateAtStep(recipient, migrateStepNames.cloned);
    joinMoveChunk();

    /**
     * Check documents are where they should be: 6 docs in each shard's respective chunk.
     */

    jsTest.log('Checking that documents are on the shards they should be...');
    assert.eq(6, donorColl.count());
    assert.eq(6, recipientColl.count());
    assert.eq(12, coll.count());

    /**
     * Check that the updated documents are where they should be, one on each shard.
     */

    jsTest.log('Checking that documents were updated correctly...');
    var donorCollUpdatedNum = donorColl.find({updatedData: "updated"}).count();
    assert.eq(1, donorCollUpdatedNum, "Update failed on donor shard during migration!");
    var recipientCollUpdatedNum = recipientColl.find({updatedData: "updated"}).count();
    assert.eq(1, recipientCollUpdatedNum, "Update failed on recipient shard during migration!");

    jsTest.log('DONE!');
    st.stop();

})();
