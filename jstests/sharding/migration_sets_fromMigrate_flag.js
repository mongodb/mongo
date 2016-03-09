//
// Tests whether the fromMigrate flag is correctly set during migrations.
//
// Tests:
//      #1 (delete op) fromMigrate is set when recipient shard deletes all documents locally
//         in the chunk range it is about to receive from the donor shard.
//      #2 (delete op) fromMigrate is set when the donor shard deletes documents that have
//         been migrated to another shard.
//      #3 (insert op) fromMigrate is set when the recipient shard receives chunk migration
//         data and inserts it.
//      #4 (update op) fromMigrate is set when an update occurs in the donor shard during
//         migration and is sent to the recipient via the transfer logs.
//      #5 fromMigrate is NOT set on donor shard and IS set on the recipient shard when real
//         delete op is done during chunk migration within the chunk range.
//

load('./jstests/libs/chunk_manipulation_util.js');

(function() {
    "use strict";

    var staticMongod = MongoRunner.runMongod({});  // For startParallelOps.

    /**
     * Start up new sharded cluster, stop balancer that would interfere in manual chunk management.
     */

    var st = new ShardingTest({shards: 2, mongos: 1, rs: {nodes: 3}});
    st.stopBalancer();

    var mongos = st.s0, admin = mongos.getDB('admin'),
        shards = mongos.getCollection('config.shards').find().toArray(), dbName = "testDB",
        ns = dbName + ".foo", coll = mongos.getCollection(ns), donor = st.shard0,
        recipient = st.shard1, donorColl = donor.getCollection(ns),
        recipientColl = recipient.getCollection(ns), donorLocal = donor.getDB('local'),
        recipientLocal = recipient.getDB('local');

    // Two chunks
    // Donor:     [0, 2) [2, 5)
    // Recipient:
    jsTest.log('Enable sharding of the collection and pre-split into two chunks....');

    assert.commandWorked(admin.runCommand({enableSharding: dbName}));
    st.ensurePrimaryShard(dbName, shards[0]._id);
    assert.commandWorked(donorColl.createIndex({_id: 1}));
    assert.commandWorked(admin.runCommand({shardCollection: ns, key: {_id: 1}}));
    assert.commandWorked(admin.runCommand({split: ns, middle: {_id: 2}}));

    // 6 documents,
    //        donor: 2 in the first chunk, 3 in the second.
    //    recipient: 1 document (shardkey overlaps with a doc in second chunk of donor shard)
    jsTest.log('Inserting 5 docs into donor shard, 1 doc into the recipient shard....');

    for (var i = 0; i < 5; ++i)
        assert.writeOK(coll.insert({_id: i}));
    assert.eq(5, donorColl.count());

    for (var i = 2; i < 3; ++i)
        assert.writeOK(recipientColl.insert({_id: i}));
    assert.eq(1, recipientColl.count());

    /**
     * Set failpoint: recipient will pause migration after cloning chunk data from donor,
     * before checking transfer mods log on donor.
     */

    jsTest.log('setting recipient failpoint cloned');
    pauseMigrateAtStep(recipient, migrateStepNames.cloned);

    /**
     * Start moving chunk [2, 5) from donor shard to recipient shard, run in the background.
     */

    // Donor:     [0, 2)
    // Recipient:    [2, 5)
    jsTest.log('Starting chunk migration, pause after cloning...');

    var joinMoveChunk = moveChunkParallel(
        staticMongod, st.s0.host, {_id: 2}, null, coll.getFullName(), shards[1]._id);

    /**
     * Wait for recipient to finish cloning.
     * THEN update 1 document {_id: 3} on donor within the currently migrating chunk.
     * AND delete 1 document {_id: 4} on donor within the currently migrating chunk.
     */

    waitForMigrateStep(recipient, migrateStepNames.cloned);

    jsTest.log('Update 1 doc and delete 1 doc on donor within the currently migrating chunk...');

    assert.writeOK(coll.update({_id: 3}, {_id: 3, a: "updated doc"}));
    assert.writeOK(coll.remove({_id: 4}));

    /**
     * Finish migration. Unpause recipient migration, wait for it to collect
     * the transfer mods log from donor and finish migration.
     */

    jsTest.log('Continuing and finishing migration...');
    unpauseMigrateAtStep(recipient, migrateStepNames.cloned);
    joinMoveChunk();

    /**
     * Check documents are where they should be: 2 docs in donor chunk, 2 docs in recipient chunk
     * (because third doc in recipient shard's chunk got deleted on the donor shard during
     * migration).
     */

    jsTest.log('Checking that documents are on the shards they should be...');

    assert.eq(2, recipientColl.count(), "Recipient shard doesn't have exactly 2 documents!");
    assert.eq(2, donorColl.count(), "Donor shard doesn't have exactly 2 documents!");
    assert.eq(4, coll.count(), "Collection total is not 4!");

    /**
     * Check that the fromMigrate flag has been set correctly in donor and recipient oplogs,
     */

    jsTest.log('Checking donor and recipient oplogs for correct fromMigrate flags...');

    var donorOplogRes = donorLocal.oplog.rs.find({op: 'd', fromMigrate: true, 'o._id': 2}).count();
    assert.eq(1,
              donorOplogRes,
              "fromMigrate flag wasn't set on the donor shard's oplog for " +
                  "migrating delete op on {_id: 2}! Test #2 failed.");

    donorOplogRes =
        donorLocal.oplog.rs.find({op: 'd', fromMigrate: {$exists: false}, 'o._id': 4}).count();
    assert.eq(1,
              donorOplogRes,
              "Real delete of {_id: 4} on donor shard incorrectly set the " +
                  "fromMigrate flag in the oplog! Test #5 failed.");

    var recipientOplogRes =
        recipientLocal.oplog.rs.find({op: 'i', fromMigrate: true, 'o._id': 2}).count();
    assert.eq(1,
              recipientOplogRes,
              "fromMigrate flag wasn't set on the recipient shard's " +
                  "oplog for migrating insert op on {_id: 2}! Test #3 failed.");

    recipientOplogRes =
        recipientLocal.oplog.rs.find({op: 'd', fromMigrate: true, 'o._id': 2}).count();
    assert.eq(1,
              recipientOplogRes,
              "fromMigrate flag wasn't set on the recipient shard's " +
                  "oplog for delete op on the old {_id: 2} that overlapped " +
                  "with the chunk about to be copied! Test #1 failed.");

    recipientOplogRes =
        recipientLocal.oplog.rs.find({op: 'u', fromMigrate: true, 'o._id': 3}).count();
    assert.eq(1,
              recipientOplogRes,
              "fromMigrate flag wasn't set on the recipient shard's " +
                  "oplog for update op on {_id: 3}! Test #4 failed.");

    recipientOplogRes =
        recipientLocal.oplog.rs.find({op: 'd', fromMigrate: true, 'o._id': 4}).count();
    assert.eq(1,
              recipientOplogRes,
              "fromMigrate flag wasn't set on the recipient shard's " +
                  "oplog for delete op on {_id: 4} that occurred during " +
                  "migration! Test #5 failed.");

    jsTest.log('DONE!');
    st.stop();

})();
