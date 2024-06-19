/**
 * Simulates a failover prior to removing the recipient doc while resharding is aborting from an
 * unrecoverable error on the donor. Resharding should abort successfully after stepUp.
 *
 * See BF-32038 for more details.
 * @tags: [
 *  requires_fcv_80,
 * ]
 */
import {DiscoverTopology} from "jstests/libs/discover_topology.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReshardingTest} from "jstests/sharding/libs/resharding_test_fixture.js";

const reshardingTest = new ReshardingTest({numDonors: 2, numRecipients: 2});
reshardingTest.setup();

const donorShardNames = reshardingTest.donorShardNames;
const recipientShardNames = reshardingTest.recipientShardNames;

const sourceCollection = reshardingTest.createShardedCollection({
    ns: "reshardingDb.coll",
    shardKeyPattern: {oldKey: 1},
    chunks: [
        {min: {oldKey: MinKey}, max: {oldKey: 10}, shard: donorShardNames[0]},
        {min: {oldKey: 10}, max: {oldKey: MaxKey}, shard: donorShardNames[1]},
    ],
});
const mongos = sourceCollection.getMongo();
const topology = DiscoverTopology.findConnectedNodes(mongos);
const donor = new Mongo(topology.shards[donorShardNames[0]].primary);
const recipient = new Mongo(topology.shards[recipientShardNames[0]].primary);

const reshardingDonorFailsBeforeObtainingTimestampFp =
    configureFailPoint(donor, "reshardingDonorFailsBeforeObtainingTimestamp");
const hangBeforeRemovingRecipientDocFp =
    configureFailPoint(recipient, "removeRecipientDocFailpoint");

reshardingTest.withReshardingInBackground(
    {
        newShardKeyPattern: {newKey: 1},
        newChunks: [
            {min: {newKey: MinKey}, max: {newKey: 10}, shard: recipientShardNames[0]},
            {min: {newKey: 10}, max: {newKey: MaxKey}, shard: recipientShardNames[1]},
        ],
    },
    () => {
        hangBeforeRemovingRecipientDocFp.wait();

        const recipientDoc =
            recipient.getCollection('config.localReshardingOperations.recipient').findOne({
                ns: "reshardingDb.coll"
            });
        assert(recipientDoc != null);
        assert(recipientDoc.mutableState.state === "done");
        assert(recipientDoc.mutableState.abortReason != null);
        assert(recipientDoc.mutableState.abortReason.code === ErrorCodes.ReshardCollectionAborted);

        reshardingTest.stepUpNewPrimaryOnShard(recipientShardNames[0]);
        const recipientRS = reshardingTest.getReplSetForShard(recipientShardNames[0]);
        recipientRS.awaitSecondaryNodes();
        recipientRS.awaitReplication();
        reshardingTest.retryOnceOnNetworkError(hangBeforeRemovingRecipientDocFp.off);
    },
    {expectedErrorCode: ErrorCodes.InternalError});

reshardingTest.teardown();
