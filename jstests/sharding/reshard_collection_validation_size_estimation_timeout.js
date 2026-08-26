/**
 * Tests that resharding skips document count validation when the collection size estimation
 * aggregation exceeds 'reshardingCollectionSizeEstimationTimeoutMS'.
 *
 * The estimation runs while the coordinator holds the FCV region. If it is not bounded, a
 * concurrent setFCV deadlocks behind it. See SERVER-133988.
 *
 * @tags: [
 *   featureFlagReshardingVerification,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReshardingTest} from "jstests/sharding/libs/resharding_test_fixture.js";

const kEstimationTimeoutMS = 1000;
const kStallDonorForMS = 30 * 1000;

function setEstimationTimeoutOnConfigNodes(configRS, timeoutMS) {
    configRS.nodes.forEach((node) => {
        assert.commandWorked(
            node.adminCommand({
                setParameter: 1,
                reshardingCollectionSizeEstimationTimeoutMS: timeoutMS,
            }),
        );
    });
}

const reshardingTest = new ReshardingTest();
reshardingTest.setup();

const configRS = reshardingTest._st.configRS;

// Resharding only runs the estimation when this parameter is on.
const verificationEnabled = assert.commandWorked(
    configRS.getPrimary().adminCommand({getParameter: 1, reshardingDocumentVerification: 1}),
).reshardingDocumentVerification;

if (!verificationEnabled) {
    jsTest.log.info("Skipping test because reshardingDocumentVerification is disabled");
    reshardingTest.teardown();
    quit();
}

const donorShardNames = reshardingTest.donorShardNames;
const recipientShardNames = reshardingTest.recipientShardNames;

const sourceCollection = reshardingTest.createShardedCollection({
    ns: "testDb.testColl",
    shardKeyPattern: {_id: 1},
    chunks: [{min: {_id: MinKey}, max: {_id: MaxKey}, shard: donorShardNames[0]}],
});
assert.commandWorked(sourceCollection.insertMany(Array.from({length: 100}, (_, i) => ({x: i}))));

const originalTimeoutMS = assert.commandWorked(
    configRS
        .getPrimary()
        .adminCommand({getParameter: 1, reshardingCollectionSizeEstimationTimeoutMS: 1}),
).reshardingCollectionSizeEstimationTimeoutMS;

setEstimationTimeoutOnConfigNodes(configRS, kEstimationTimeoutMS);

// Stall the estimation's aggregation on the donor. Every node is covered because the estimation
// does not always target the primary.
const blockAggregate = reshardingTest.getReplSetForShard(donorShardNames[0]).nodes.map((node) =>
    configureFailPoint(node, "failCommand", {
        failCommands: ["aggregate"],
        namespace: sourceCollection.getFullName(),
        blockConnection: true,
        blockTimeMS: kStallDonorForMS,
        failInternalCommands: true,
    }),
);

try {
    reshardingTest.withReshardingInBackground(
        {
            newShardKeyPattern: {x: 1},
            newChunks: [{min: {x: MinKey}, max: {x: MaxKey}, shard: recipientShardNames[0]}],
        },
        () => {
            // The coordinator document is written once the estimation times out, so check that
            // verification was skipped.
            assert.soon(() => {
                const coordinatorDoc = reshardingTest._st.config.reshardingOperations.findOne({
                    ns: sourceCollection.getFullName(),
                });
                return coordinatorDoc !== null && coordinatorDoc.performVerification === false;
            }, "expected verification to be skipped after the size estimation timed out");

            // Let the donor serve aggregations again so that resharding can clone.
            blockAggregate.forEach((fp) => fp.off());
        },
    );
} finally {
    blockAggregate.forEach((fp) => fp.off());
    setEstimationTimeoutOnConfigNodes(configRS, originalTimeoutMS);
}

reshardingTest.teardown();
