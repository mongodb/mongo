/**
 * Tests that chunk migrations, collMod, createIndexes, and dropIndexes are prohibited on a
 * collection that is undergoing a resharding operation. Also tests that concurrent resharding
 * operations are prohibited.
 *
 * @tags: [
 *   uses_atclustertime,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/discover_topology.js");
load("jstests/sharding/libs/resharding_test_fixture.js");
load("jstests/libs/fail_point_util.js");
load('jstests/libs/parallel_shell_helpers.js');

const reshardingTest = new ReshardingTest({numDonors: 2});
reshardingTest.setup();

const donorShardNames = reshardingTest.donorShardNames;
let sourceCollection = reshardingTest.createShardedCollection({
    ns: "reshardingDb.coll",
    shardKeyPattern: {oldKey: 1},
    chunks: [
        {min: {oldKey: MinKey}, max: {oldKey: 0}, shard: donorShardNames[0]},
        {min: {oldKey: 0}, max: {oldKey: MaxKey}, shard: donorShardNames[1]}
    ],
});

const recipientShardNames = reshardingTest.recipientShardNames;
const mongos = sourceCollection.getMongo();
let ns = sourceCollection.getFullName();

const topology = DiscoverTopology.findConnectedNodes(mongos);
const configPrimary = new Mongo(topology.configsvr.primary);
const donor0 = new Mongo(topology.shards[donorShardNames[0]].primary);
const donor1 = new Mongo(topology.shards[donorShardNames[1]].primary);

// TODO SERVER-58950: Remove all calls to isMixedVersionCluster() in this test
let pauseCoordinatorBeforeRemovingStateDoc;
let pauseBeforeRemoveDonor0Doc;
let pauseBeforeRemoveDonor1Doc;
if (!reshardingTest.isMixedVersionCluster()) {
    pauseCoordinatorBeforeRemovingStateDoc =
        configureFailPoint(configPrimary, "reshardingPauseCoordinatorBeforeRemovingStateDoc");

    pauseBeforeRemoveDonor0Doc = configureFailPoint(donor0, "removeDonorDocFailpoint");
    pauseBeforeRemoveDonor1Doc = configureFailPoint(donor1, "removeDonorDocFailpoint");
}

let awaitAbort;
reshardingTest.withReshardingInBackground(
    {
        newShardKeyPattern: {newKey: 1},
        newChunks: [{min: {newKey: MinKey}, max: {newKey: MaxKey}, shard: recipientShardNames[0]}],
    },
    (tempNs) => {
        const topology = DiscoverTopology.findConnectedNodes(mongos);
        assert.soon(() => {
            let res =
                donor0.getCollection("config.localReshardingOperations.donor").find().toArray();
            return res.length == 1;
        }, "timed out waiting for resharding initialization on donor shard");

        awaitAbort = startParallelShell(
            funWithArgs(function(ns) {
                assert.commandWorked(db.adminCommand({abortReshardCollection: ns}));
            }, ns), mongos.port);
    },
    {
        expectedErrorCode: ErrorCodes.ReshardCollectionAborted,
        postDecisionPersistedFn: () => {
            if (!reshardingTest.isMixedVersionCluster()) {
                // Once the coordinator has persisted an abort decision, collMod, createIndexes, and
                // dropIndexes should be able to succeed. Wait until both donors have heard that the
                // coordinator has made the decision.
                assert.soon(() => {
                    const res0 =
                        donor0.getCollection("config.cache.collections").findOne({_id: ns});
                    const res1 =
                        donor1.getCollection("config.cache.collections").findOne({_id: ns});
                    return res0.reshardingFields.state === "aborting" &&
                        res1.reshardingFields.state === "aborting";
                }, () => `timed out waiting for the coordinator to persist decision`);

                const db = mongos.getDB("reshardingDb");
                assert.commandWorked(db.runCommand({collMod: 'coll'}));
                assert.commandWorked(db.runCommand({
                    createIndexes: 'coll',
                    indexes: [{name: "idx1", key: {newKey: 1, oldKey: 1}}]
                }));
                assert.commandWorked(
                    db.runCommand({dropIndexes: 'coll', index: {newKey: 1, oldKey: 1}}));

                pauseBeforeRemoveDonor0Doc.off();
                pauseBeforeRemoveDonor1Doc.off();
                pauseCoordinatorBeforeRemovingStateDoc.off();
                awaitAbort();
            }
        }
    });

if (!reshardingTest.isMixedVersionCluster()) {
    pauseCoordinatorBeforeRemovingStateDoc =
        configureFailPoint(configPrimary, "reshardingPauseCoordinatorBeforeRemovingStateDoc");
}

let pauseBeforeRemoveRecipientDoc;
reshardingTest.withReshardingInBackground(
    {
        newShardKeyPattern: {newKey: 1},
        newChunks: [{min: {newKey: MinKey}, max: {newKey: MaxKey}, shard: recipientShardNames[0]}],
    },
    (tempNs) => {
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

        const topology = DiscoverTopology.findConnectedNodes(mongos);
        const donor = new Mongo(topology.shards[donorShardNames[0]].primary);
        assert.soon(() => {
            res = donor.getCollection("config.localReshardingOperations.donor").find().toArray();
            return res.length == 1;
        }, "timed out waiting for resharding initialization on donor shard");

        assert.commandFailedWithCode(
            mongos.adminCommand({moveChunk: ns, find: {oldKey: -10}, to: donorShardNames[1]}),
            ErrorCodes.LockBusy);
        assert.commandFailedWithCode(db.runCommand({collMod: 'coll'}),
                                     ErrorCodes.ReshardCollectionInProgress);
        assert.commandFailedWithCode(sourceCollection.createIndexes([{newKey: 1}]),
                                     ErrorCodes.ReshardCollectionInProgress);
        assert.commandFailedWithCode(db.runCommand({dropIndexes: 'coll', index: '*'}),
                                     ErrorCodes.ReshardCollectionInProgress);

        let newNs = "reshardingDb2.coll2";
        assert.commandWorked(mongos.adminCommand({enableSharding: "reshardingDb2"}));
        assert.commandWorked(mongos.adminCommand({shardCollection: newNs, key: {oldKey: 1}}));

        assert.commandFailedWithCode(
            mongos.adminCommand({reshardCollection: newNs, key: {newKey: 1}}),
            ErrorCodes.ReshardCollectionInProgress);

        mongos.getCollection(newNs).drop();

        if (!reshardingTest.isMixedVersionCluster()) {
            const recipient = new Mongo(topology.shards[recipientShardNames[0]].primary);
            pauseBeforeRemoveRecipientDoc =
                configureFailPoint(recipient, "removeRecipientDocFailpoint");
        }
    },
    {
        postDecisionPersistedFn: () => {
            if (!reshardingTest.isMixedVersionCluster()) {
                // Once the coordinator has persisted a commit decision, collMod, createIndexes, and
                // dropIndexes should be able to succeed. Wait until the recipient is aware that the
                // coordinator has persisted the decision.
                const recipient = new Mongo(topology.shards[recipientShardNames[0]].primary);
                assert.soon(
                    () => {
                        const res =
                            recipient.getCollection("config.cache.collections").findOne({_id: ns});
                        return res.reshardingFields.state === "committing";
                    },
                    () => `timed out waiting for the coordinator to persist decision: ${
                        tojson(res)}`);

                const db = mongos.getDB("reshardingDb");
                assert.commandWorked(db.runCommand({collMod: 'coll'}));

                // Create two indexes - one (idx1) that we will drop right away to ensure that
                // dropIndexes succeeds, and the other (idx2) we will check still exists after the
                // collection has been renamed.
                assert.commandWorked(db.runCommand({
                    createIndexes: 'coll',
                    indexes: [{name: "idx1", key: {newKey: 1, oldKey: 1}}]
                }));
                assert.commandWorked(db.runCommand(
                    {createIndexes: 'coll', indexes: [{name: "idx2", key: {newKey: 1, x: 1}}]}));

                assert.commandWorked(
                    db.runCommand({dropIndexes: 'coll', index: {newKey: 1, oldKey: 1}}));

                pauseBeforeRemoveRecipientDoc.off();
                pauseCoordinatorBeforeRemovingStateDoc.off();
            }
        }
    });

// Assert that 'idx2' still exists after we've renamed the collection and resharding is finished.
if (!reshardingTest.isMixedVersionCluster()) {
    let indexes = mongos.getDB("reshardingDb").runCommand({listIndexes: 'coll'});
    assert(indexes.cursor.firstBatch.some((index) => index.name === 'idx2'));
}

reshardingTest.teardown();
})();
