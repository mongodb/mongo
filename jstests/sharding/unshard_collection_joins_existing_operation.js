/**
 * Tests that if an unshardCollection command is issued while there is an ongoing unshardCollection
 * operation for the same collection with the same destination shard, the command joins with the
 * ongoing unshardCollection instance.
 *
 * @tags: [
 *   does_not_support_stepdowns,
 *   featureFlagUnshardCollection,
 *   requires_fcv_80,
 *   uses_atclustertime,
 * ]
 */
import {Thread} from "jstests/libs/parallelTester.js";
import {ReshardingTest} from "jstests/sharding/libs/resharding_test_fixture.js";
import {runJoinsExistingOperationTest} from "jstests/sharding/libs/resharding_test_joins_operation.js";

// Generates a new thread to run unshardCollection.
const makeUnshardCollectionThread = (mongosHost, ns, extraArgs) => {
    return new Thread(
        (mongosHost, ns, toShard) => {
            const mongoS = new Mongo(mongosHost);
            assert.commandWorked(mongoS.adminCommand({unshardCollection: ns, toShard: toShard}));
        },
        mongosHost,
        ns,
        extraArgs.toShard,
    );
};

const reshardingTest = new ReshardingTest();
reshardingTest.setup();
const donorShardNames = reshardingTest.donorShardNames;
const recipientShardNames = reshardingTest.recipientShardNames;

// Create a sharded collection - unshardCollection requires a sharded collection as input
const sourceCollection = reshardingTest.createShardedCollection({
    ns: "reshardingDb.coll",
    shardKeyPattern: {oldKey: 1},
    chunks: [{min: {oldKey: MinKey}, max: {oldKey: MaxKey}, shard: donorShardNames[0]}],
});

runJoinsExistingOperationTest(reshardingTest, {
    opType: "unshardCollection",
    operationArgs: {toShard: recipientShardNames[0]},
    makeJoiningThreadFn: makeUnshardCollectionThread,
    joiningThreadExtraArgs: {toShard: recipientShardNames[0]},
});

reshardingTest.teardown();
