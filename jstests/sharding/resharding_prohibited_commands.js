/**
 * Tests that chunk migrations, collMod, createIndexes, and dropIndexes are prohibited on a
 * collection that is undergoing a resharding operation.
 *
 * @tags: [
 *   requires_fcv_49,
 *   uses_atclustertime,
 * ]
 */
(function() {
"use strict";

load("jstests/sharding/libs/resharding_test_fixture.js");

const reshardingTest = new ReshardingTest({numDonors: 2});
reshardingTest.setup();

const donorShardNames = reshardingTest.donorShardNames;
const sourceCollection = reshardingTest.createShardedCollection({
    ns: "reshardingDb.coll",
    shardKeyPattern: {oldKey: 1},
    chunks: [{min: {oldKey: MinKey}, max: {oldKey: MaxKey}, shard: donorShardNames[0]}],
});

const recipientShardNames = reshardingTest.recipientShardNames;
reshardingTest.withReshardingInBackground(
    {
        newShardKeyPattern: {newKey: 1},
        newChunks: [{min: {newKey: MinKey}, max: {newKey: MaxKey}, shard: recipientShardNames[0]}],
    },
    (tempNs) => {
        const mongos = sourceCollection.getMongo();
        const ns = sourceCollection.getFullName();
        const db = sourceCollection.getDB();

        let res;
        assert.soon(() => {
            res = mongos.getCollection("config.collections")
                      .find({_id: {$in: [ns, tempNs]}})
                      .toArray();

            return res.length === 2 && res.every(collEntry => collEntry.allowMigrations === false);
        }, () => `timed out waiting for collections to have allowMigrations=false: ${tojson(res)}`);
        assert.soon(
            () => {
                res = mongos.getCollection("config.collections").findOne({_id: ns});
                return res.hasOwnProperty("reshardingFields");
            },
            () => `timed out waiting for resharding fields to be added to original nss: ${
                tojson(res)}`);

        assert.commandFailedWithCode(
            mongos.adminCommand({moveChunk: ns, find: {oldKey: -10}, to: donorShardNames[1]}),
            ErrorCodes.ConflictingOperationInProgress);
        assert.commandFailedWithCode(db.runCommand({collMod: 'coll'}),
                                     ErrorCodes.ReshardCollectionInProgress);
        assert.commandFailedWithCode(sourceCollection.createIndexes([{newKey: 1}]),
                                     ErrorCodes.ReshardCollectionInProgress);
        assert.commandFailedWithCode(db.runCommand({dropIndexes: 'coll', index: '*'}),
                                     ErrorCodes.ReshardCollectionInProgress);
    });

reshardingTest.teardown();
})();
