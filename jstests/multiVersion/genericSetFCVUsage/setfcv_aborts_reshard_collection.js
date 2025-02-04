/**
 * Tests that setFeatureCompatibilityVersion command aborts an ongoing reshardCollection command
 */
import {DiscoverTopology} from "jstests/libs/discover_topology.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ReshardingTest} from "jstests/sharding/libs/resharding_test_fixture.js";
import {waitForFailpoint} from "jstests/sharding/libs/sharded_transactions_helpers.js";

// Global variable is used to avoid spinning up a set of servers just to see if the
// feature flag is enabled.
let reshardingImprovementsEnabled;
function runTest({forcePooledConnectionsDropped, withUUID}) {
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

    if (reshardingImprovementsEnabled === undefined) {
        reshardingImprovementsEnabled = FeatureFlagUtil.isEnabled(mongos, "ReshardingImprovements");
    }
    if (withUUID && !reshardingImprovementsEnabled) {
        jsTestLog("Skipping test with UUID since featureFlagReshardingImprovements is not enabled");
        reshardingTest.tearDown();
    }
    jsTestLog("Testing with forcePooledConnectionsDropped: " + forcePooledConnectionsDropped +
              " withUUID: " + withUUID);

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

            return coordinatorDoc === null || coordinatorDoc.state === "aborting" ||
                coordinatorDoc.state === "quiesced";
        });
    }

    const recipientShardNames = reshardingTest.recipientShardNames;
    let awaitShell;
    let reshardingUUID = withUUID ? UUID() : undefined;
    reshardingTest.withReshardingInBackground(
        {
            newShardKeyPattern: {newKey: 1},
            newChunks: [
                {min: {newKey: MinKey}, max: {newKey: 0}, shard: recipientShardNames[0]},
                {min: {newKey: 0}, max: {newKey: MaxKey}, shard: recipientShardNames[1]},
            ],
            reshardingUUID: reshardingUUID,
            // 'performVerification' defaults to true which is only supported in FCV 'latest' and
            // this test case downgrades the FCV which causes the reshardCollection command to fail
            // with an InvalidOptions error right away.
            performVerification: false,
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
                assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}));
            }`;

            awaitShell = startParallelShell(codeToRunInParallelShell, mongos.port);

            if (forcePooledConnectionsDropped) {
                pauseBeforeCloseCxns.wait();

                let pauseBeforeMarkKeepOpen = configureFailPoint(config, "pauseBeforeMarkKeepOpen");

                pauseBeforeTellDonorToRefresh.off();

                jsTestLog("Wait to hit pauseBeforeMarkKeepOpen failpoint");
                pauseBeforeMarkKeepOpen.wait();

                jsTestLog("Set hitDropConnections failpoint");
                let hitDropConnections =
                    configureFailPoint(config, "finishedDropConnections", {}, {times: 1});
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
                // The query feature used in resharding can be disallowed after FCV downgrade,
                // resulting in an InvalidOptions error.
                ErrorCodes.InvalidOptions,
                // setFCV will abort index build and resharding. Since resharding can also be
                // building index, it is possible that the index build gets aborted first and
                // resharding fails on IndexBuildAborted.
                ErrorCodes.IndexBuildAborted,
                // The use of $_requestResumeToken can fail after downgrade because resharding
                // improvements are not enabled, which produces this specific error code.
                90675,
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
            awaitShell = startParallelShell(
                funWithArgs(function(latestFCV) {
                    assert.commandWorked(db.adminCommand(
                        {setFeatureCompatibilityVersion: latestFCV, confirm: true}));
                }, latestFCV), mongos.port);
            checkCoordinatorDoc();
        },
        {
            expectedErrorCode: [
                ErrorCodes.CommandNotSupported,
                ErrorCodes.ReshardCollectionAborted,
                ErrorCodes.Interrupted,
                // setFCV will abort index build and resharding. Since resharding can also be
                // building index, it is possible that the index build gets aborted first and
                // resharding fails on IndexBuildAborted.
                ErrorCodes.IndexBuildAborted,
            ]
        });

    awaitShell();
    reshardingTest.teardown();
}

// This test case forces the setFCV command to call dropsConnections while the coordinator is in
// the process of establishing connections to the participant shards in order to ensure that the
// resharding operation does not stall.
runTest({forcePooledConnectionsDropped: true});

assert(reshardingImprovementsEnabled !== undefined);

// We test with a UUID because we need for setFCV to abort the quiesce period as well, in order
// to completely clear the config server's state collection.  Because this test takes a while
// we don't try all combinations of forcePooledCollectionsDropped and withUUID.
runTest({forcePooledConnectionsDropped: false, withUUID: reshardingImprovementsEnabled});
