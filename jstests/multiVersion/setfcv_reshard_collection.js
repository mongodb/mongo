(function() {
"use strict";

load("jstests/sharding/libs/resharding_test_fixture.js");

const reshardingTest = new ReshardingTest({numDonors: 2, numRecipients: 2, reshardInPlace: true});
reshardingTest.setup();

const donorShardNames = reshardingTest.donorShardNames;
let inputCollection = reshardingTest.createShardedCollection({
    ns: "reshardingDb.testColl",
    shardKeyPattern: {oldKey: 1},
    chunks: [
        {min: {oldKey: MinKey}, max: {oldKey: 0}, shard: donorShardNames[0]},
        {min: {oldKey: 0}, max: {oldKey: MaxKey}, shard: donorShardNames[1]},
    ],
});

let mongos = inputCollection.getMongo();

for (let x = 0; x < 1000; x++) {
    assert.commandWorked(inputCollection.insert({oldKey: x, newKey: -1 * x}));
}

const recipientShardNames = reshardingTest.recipientShardNames;
reshardingTest.withReshardingInBackground(
    {
        newShardKeyPattern: {newKey: 1},
        newChunks: [
            {min: {newKey: MinKey}, max: {newKey: 0}, shard: recipientShardNames[0]},
            {min: {newKey: 0}, max: {newKey: MaxKey}, shard: recipientShardNames[1]},
        ],
    },
    () => {
        // Wait for config server to have started resharding before sending setFCV, otherwise
        // there is a possible race where setFCV can be sent to the config before configsvrReshard.
        assert.soon(() => {
            return mongos.getDB('config').reshardingOperations.findOne() != null;
        }, "timed out waiting for coordinator doc to be written", 30 * 1000);

        assert.commandWorked(mongos.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));
    },
    // setFCV closes all connection, so reshardCollection can sometimes end up getting
    // PooledConnectionsDropped error instead.
    {
        expectedErrorCode: [
            ErrorCodes.ReshardCollectionAborted,
            ErrorCodes.PooledConnectionsDropped,
            ErrorCodes.Interrupted,
            // TODO: SERVER-55912 this is only for downgrade to < v4.7, which makes namespaces
            // used by resharding invalid.
            ErrorCodes.InvalidNamespace,
        ]
    });

// TODO SERVER-55912: replace with test case that run resharding in background and setFCV to
// latestFCV.
reshardingTest.withReshardingInBackground({
    newShardKeyPattern: {newKey: 1},
    newChunks: [
        {min: {newKey: MinKey}, max: {newKey: 0}, shard: recipientShardNames[0]},
        {min: {newKey: 0}, max: {newKey: MaxKey}, shard: recipientShardNames[1]},
    ],
},
                                          () => {},
                                          {expectedErrorCode: ErrorCodes.CommandNotSupported});

reshardingTest.teardown();
})();
