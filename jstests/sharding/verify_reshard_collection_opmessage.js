/**
 * Test that an opMessage is created during reshardCollection command.
 *
 * @tags: [requires_fcv_60, __TEMPORARILY_DISABLED__]
 */
(function() {
"use strict";
load("jstests/libs/uuid_util.js");
load("jstests/sharding/libs/resharding_test_fixture.js");

const sourceNs = "reshardingDb.coll";

const reshardingTest = new ReshardingTest({
    numDonors: 2,
    numRecipients: 2,
    reshardInPlace: true,
});
reshardingTest.setup();

const donorShardNames = reshardingTest.donorShardNames;
const recipientShardNames = reshardingTest.recipientShardNames;

const primaryShardName = donorShardNames[0];

const inputCollection = reshardingTest.createShardedCollection({
    ns: sourceNs,
    shardKeyPattern: {oldKey: 1},
    primaryShardName: primaryShardName,
    chunks: [
        {min: {oldKey: MinKey}, max: {oldKey: 0}, shard: donorShardNames[0]},
        {min: {oldKey: 0}, max: {oldKey: MaxKey}, shard: donorShardNames[1]},
    ],
});

const mongos = inputCollection.getMongo();
const collectionUUID = getUUIDFromConfigCollections(mongos, sourceNs);

reshardingTest.withReshardingInBackground({
    newShardKeyPattern: {newKey: 1},
    newChunks: [
        {min: {newKey: MinKey}, max: {newKey: 0}, shard: recipientShardNames[0]},
        {min: {newKey: 0}, max: {newKey: MaxKey}, shard: recipientShardNames[1]},
    ],
});

const oplog = reshardingTest.getReplSetForShard(primaryShardName)
                  .getPrimary()
                  .getCollection("local.oplog.rs");
const logEntry = oplog.findOne({ns: sourceNs, op: 'n', "o2.reshardCollection": sourceNs});
assert(logEntry != null);
const reshardedUUID = getUUIDFromConfigCollections(mongos, sourceNs);
assert.eq(reshardedUUID, logEntry.o2.reshardUUID, logEntry);
assert.eq(logEntry.ui, collectionUUID, logEntry);
assert.eq(bsonWoCompare(logEntry.o2.oldShardKey, {oldKey: 1}), 0, logEntry);
assert.eq(bsonWoCompare(logEntry.o2.shardKey, {newKey: 1}), 0, logEntry);

reshardingTest.teardown();
})();
