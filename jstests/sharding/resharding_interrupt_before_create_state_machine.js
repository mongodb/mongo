/**
 * Test that reshardCollection does not hang if its opCtx is interrupted between inserting the state
 * document for its state machine and starting that state machine. See SERVER-74647.
 */
(function() {
"use strict";
load("jstests/sharding/libs/resharding_test_fixture.js");
load("jstests/libs/fail_point_util.js");
load("jstests/libs/discover_topology.js");

const sourceNs = "reshardingDb.coll";

const reshardingTest = new ReshardingTest();
reshardingTest.setup();

const donorShardNames = reshardingTest.donorShardNames;
const recipientShardNames = reshardingTest.recipientShardNames;

const inputCollection = reshardingTest.createShardedCollection({
    ns: sourceNs,
    shardKeyPattern: {oldKey: 1},
    chunks: [
        {min: {oldKey: MinKey}, max: {oldKey: MaxKey}, shard: donorShardNames[0]},
    ],
});

const mongos = inputCollection.getMongo();
const topology = DiscoverTopology.findConnectedNodes(mongos);
const donorPrimary = new Mongo(topology.shards[donorShardNames[0]].primary);

const failpoint =
    configureFailPoint(donorPrimary, "reshardingInterruptAfterInsertStateMachineDocument");

reshardingTest.withReshardingInBackground({
    newShardKeyPattern: {newKey: 1},
    newChunks: [
        {min: {newKey: MinKey}, max: {newKey: MaxKey}, shard: recipientShardNames[0]},
    ]
},
                                          () => {
                                              failpoint.wait();
                                              failpoint.off();
                                          });

reshardingTest.teardown();
})();
