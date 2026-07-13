/**
 * Tests that resharding skips document count validation when the estimated collection size exceeds
 * the 'reshardingDocumentValidationMaxCollectionSizeBytes' server parameter.
 *
 * @tags: [
 *   cannot_run_during_upgrade_downgrade,
 *   featureFlagReshardingVerification,
 *   requires_fcv_90,
 * ]
 */

import {ReshardingTest} from "jstests/sharding/libs/resharding_test_fixture.js";

const reshardingTest = new ReshardingTest();
reshardingTest.setup();

const donorShardNames = reshardingTest.donorShardNames;
const recipientShardNames = reshardingTest.recipientShardNames;

const sourceCollection = reshardingTest.createShardedCollection({
    ns: "testDb.testColl",
    shardKeyPattern: {_id: 1},
    chunks: [{min: {_id: MinKey}, max: {_id: MaxKey}, shard: donorShardNames[0]}],
});
assert.commandWorked(sourceCollection.insertMany(Array.from({length: 100}, (_, i) => ({x: i}))));

// Save the current threshold and set it to 1 byte on all config RS nodes so any non-empty
// collection exceeds it.
const configRS = reshardingTest._st.configRS;
const originalThreshold = assert.commandWorked(
    configRS
        .getPrimary()
        .adminCommand({getParameter: 1, reshardingDocumentValidationMaxCollectionSizeBytes: 1}),
).reshardingDocumentValidationMaxCollectionSizeBytes;

configRS.nodes.forEach((node) => {
    assert.commandWorked(
        node.adminCommand({setParameter: 1, reshardingDocumentValidationMaxCollectionSizeBytes: 1}),
    );
});

try {
    reshardingTest.withReshardingInBackground(
        {
            newShardKeyPattern: {x: 1},
            newChunks: [{min: {x: MinKey}, max: {x: MaxKey}, shard: recipientShardNames[0]}],
        },
        () => {},
        {
            postCheckConsistencyFn: () => {
                const coordinatorDoc = reshardingTest._st.config.reshardingOperations.findOne({
                    ns: sourceCollection.getFullName(),
                });
                assert.eq(coordinatorDoc.performVerification, false, coordinatorDoc);
            },
        },
    );
} finally {
    configRS.nodes.forEach((node) => {
        assert.commandWorked(
            node.adminCommand({
                setParameter: 1,
                reshardingDocumentValidationMaxCollectionSizeBytes: originalThreshold,
            }),
        );
    });
}

reshardingTest.teardown();
