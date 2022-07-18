/**
 * Tests that setFeatureCompatibilityVersion command aborts an ongoing reshardCollection command
 */
(function() {
"use strict";

load("jstests/libs/parallel_shell_helpers.js");
load("jstests/sharding/libs/resharding_test_fixture.js");
load('jstests/libs/discover_topology.js');
load('jstests/libs/fail_point_util.js');
load('jstests/sharding/libs/sharded_transactions_helpers.js');

function runTest(forcePooledConnectionsDropped) {
    const reshardingTest =
        new ReshardingTest({numDonors: 2, numRecipients: 2, reshardInPlace: true});
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

    const sourceNamespace = inputCollection.getFullName();

    let mongos = inputCollection.getMongo();

    for (let x = 0; x < 1000; x++) {
        assert.commandWorked(inputCollection.insert({oldKey: x, newKey: -1 * x}));
    }

    const topology = DiscoverTopology.findConnectedNodes(mongos);
    const config = new Mongo(topology.configsvr.primary);

    let pauseBeforeTellDonorToRefresh;
    let pauseBeforeCloseCxns;
    if (forcePooledConnectionsDropped) {
        pauseBeforeTellDonorToRefresh = configureFailPoint(config, "pauseBeforeTellDonorToRefresh");
        pauseBeforeCloseCxns = configureFailPoint(config, "pauseBeforeCloseCxns");
    }

    function checkCoordinatorDoc() {
        assert.soon(() => {
            const coordinatorDoc =
                mongos.getCollection("config.reshardingOperations").findOne({ns: sourceNamespace});

            return coordinatorDoc === null || coordinatorDoc.state === "aborting";
        });
    }

    const recipientShardNames = reshardingTest.recipientShardNames;
    let awaitShell;
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
            // there is a possible race where setFCV can be sent to the config before
            // configsvrReshard.
            assert.soon(() => {
                return mongos.getDB('config').reshardingOperations.findOne() != null;
            }, "timed out waiting for coordinator doc to be written", 30 * 1000);

            if (forcePooledConnectionsDropped) {
                pauseBeforeTellDonorToRefresh.wait();
            }

            let codeToRunInParallelShell =
                `{
                assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));
            }`;

            awaitShell = startParallelShell(codeToRunInParallelShell, mongos.port);

            if (forcePooledConnectionsDropped) {
                pauseBeforeCloseCxns.wait();

                let pauseBeforeMarkKeepOpen = configureFailPoint(config, "pauseBeforeMarkKeepOpen");

                pauseBeforeTellDonorToRefresh.off();

                jsTestLog("Wait to hit pauseBeforeMarkKeepOpen failpoint");
                pauseBeforeMarkKeepOpen.wait();

                jsTestLog("Set hitDropConnections failpoint");
                let hitDropConnections = configureFailPoint(config, "finishedDropConnections");
                pauseBeforeCloseCxns.off();

                waitForFailpoint("Hit finishedDropConnections", 1);
                clearRawMongoProgramOutput();

                jsTestLog("Turn off hitDropConnections failpoint");
                hitDropConnections.off();

                jsTestLog("Turn off pause before pauseBeforeMarkKeepOpen failpoint");
                pauseBeforeMarkKeepOpen.off();
            }
            checkCoordinatorDoc();
        },
        {
            expectedErrorCode: [
                ErrorCodes.ReshardCollectionAborted,
                ErrorCodes.Interrupted,
            ]
        });

    awaitShell();

    reshardingTest.withReshardingInBackground(
        {
            newShardKeyPattern: {newKey: 1},
            newChunks: [
                {min: {newKey: MinKey}, max: {newKey: 0}, shard: recipientShardNames[0]},
                {min: {newKey: 0}, max: {newKey: MaxKey}, shard: recipientShardNames[1]},
            ],
        },
        () => {
            assert.soon(() => {
                return mongos.getDB('config').reshardingOperations.findOne() != null;
            }, "timed out waiting for coordinator doc to be written", 30 * 1000);
            awaitShell = startParallelShell(funWithArgs(function(latestFCV) {
                                                assert.commandWorked(db.adminCommand(
                                                    {setFeatureCompatibilityVersion: latestFCV}));
                                            }, latestFCV), mongos.port);
            checkCoordinatorDoc();
        },
        {
            expectedErrorCode: [
                ErrorCodes.CommandNotSupported,
                ErrorCodes.ReshardCollectionAborted,
                ErrorCodes.Interrupted,
            ]
        });

    awaitShell();
    reshardingTest.teardown();
}

// This test case forces the setFCV command to call dropsConnections while the coordinator is in
// the process of establishing connections to the participant shards in order to ensure that the
// resharding operation does not stall.
runTest(true);

runTest(false);
})();
