/**
 * Tests that if a reshardCollection command is issued while there is an ongoing resharding
 * operation for the same collection with the same resharding key, the command joins with the
 * ongoing resharding instance.
 *
 * @tags: [
 *   does_not_support_stepdowns,
 *   uses_atclustertime,
 * ]
 */
import {Thread} from "jstests/libs/parallelTester.js";
import {ReshardingTest} from "jstests/sharding/libs/resharding_test_fixture.js";
import {runJoinsExistingOperationTest} from "jstests/sharding/libs/resharding_test_joins_operation.js";

// Generates a new thread to run reshardCollection.
// The joining command must pass the same arguments as the background operation to be recognized
// as a duplicate that should join rather than conflict. This includes _presetReshardedChunks.
const makeReshardCollectionThread = (mongosHost, ns, extraArgs) => {
    return new Thread(
        (mongosHost, ns, newShardKey, presetReshardedChunks) => {
            const mongoS = new Mongo(mongosHost);
            assert.commandWorked(
                mongoS.adminCommand({
                    reshardCollection: ns,
                    key: newShardKey,
                    _presetReshardedChunks: presetReshardedChunks,
                }),
            );
        },
        mongosHost,
        ns,
        extraArgs.newShardKey,
        extraArgs.presetReshardedChunks,
    );
};

const reshardingTest = new ReshardingTest({numDonors: 1});
reshardingTest.setup();
const donorShardNames = reshardingTest.donorShardNames;
const recipientShardNames = reshardingTest.recipientShardNames;
const sourceCollection = reshardingTest.createShardedCollection({
    ns: "reshardingDb.coll",
    shardKeyPattern: {oldKey: 1},
    chunks: [{min: {oldKey: MinKey}, max: {oldKey: MaxKey}, shard: donorShardNames[0]}],
});

// The newChunks format uses 'shard' but the command expects 'recipientShardId'.
const newChunks = [{min: {newKey: MinKey}, max: {newKey: MaxKey}, shard: recipientShardNames[0]}];
const presetReshardedChunks = newChunks.map((chunk) => ({
    min: chunk.min,
    max: chunk.max,
    recipientShardId: chunk.shard,
}));

runJoinsExistingOperationTest(reshardingTest, {
    opType: "reshardCollection",
    operationArgs: {
        newShardKeyPattern: {newKey: 1},
        newChunks: newChunks,
    },
    makeJoiningThreadFn: makeReshardCollectionThread,
    joiningThreadExtraArgs: {newShardKey: {newKey: 1}, presetReshardedChunks: presetReshardedChunks},
});

reshardingTest.teardown();
