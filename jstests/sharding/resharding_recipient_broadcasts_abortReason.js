/**
 * Tests that when a donor shard encounters an unrecoverable error, the error gets propagated from
 * the failing donor shard, to the coordinator, and then to the remaining participant shards.
 *
 * @tags: [
 *   requires_fcv_49,
 *   uses_atclustertime,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/discover_topology.js");
load("jstests/sharding/libs/resharding_test_fixture.js");
load("jstests/sharding/libs/resharding_test_util.js");

const reshardingTest = new ReshardingTest({numDonors: 2, numRecipients: 2});

reshardingTest.setup();

const donorShardNames = reshardingTest.donorShardNames;
const inputCollection = reshardingTest.createShardedCollection({
    ns: "reshardingDb.coll",
    shardKeyPattern: {oldKey: 1},
    chunks: [
        {min: {oldKey: MinKey}, max: {oldKey: 0}, shard: donorShardNames[0]},
        {min: {oldKey: 0}, max: {oldKey: MaxKey}, shard: donorShardNames[1]},
    ],
});

assert.commandWorked(inputCollection.insert([
    {_id: "stays on shard0", oldKey: -10, newKey: -10},
    {_id: "moves to shard0", oldKey: 10, newKey: -10},
    {_id: "moves to shard1", oldKey: -10, newKey: 10},
    {_id: "stays on shard1", oldKey: 10, newKey: 10},
]));

const mongos = inputCollection.getMongo();
const recipientShardNames = reshardingTest.recipientShardNames;

const topology = DiscoverTopology.findConnectedNodes(mongos);
const donor_host = topology.shards[donorShardNames[0]].primary;
const donor0 = new Mongo(donor_host);
const configsvr = new Mongo(topology.configsvr.nodes[0]);

const failBeforeMirroringFP =
    configureFailPoint(donor0, "reshardingDonorFailsBeforePreparingToMirror");
const removeDonorDocFP = configureFailPoint(donor0, "removeDonorDocFailpoint");

reshardingTest.withReshardingInBackground(
    {
        newShardKeyPattern: {newKey: 1},
        newChunks: [
            {min: {newKey: MinKey}, max: {newKey: 0}, shard: recipientShardNames[0]},
            {min: {newKey: 0}, max: {newKey: MaxKey}, shard: recipientShardNames[1]},
        ],
    },
    () => {},
    {
        expectedErrorCode: ErrorCodes.InternalError,
        postAbortDecisionPersistedFn: () => {
            ReshardingTestUtil.assertDonorAbortsLocally(donor0,
                                                        donorShardNames[0],
                                                        inputCollection.getFullName(),
                                                        ErrorCodes.InternalError);

            removeDonorDocFP.off();

            ReshardingTestUtil.assertAllParticipantsReportDoneToCoordinator(
                configsvr, inputCollection.getFullName());
        }
    });

failBeforeMirroringFP.off();
reshardingTest.teardown();
})();
