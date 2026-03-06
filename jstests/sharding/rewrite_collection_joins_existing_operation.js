/**
 * Tests that if a rewriteCollection command is issued while there is an ongoing rewriteCollection
 * operation for the same collection, the command joins with the ongoing rewriteCollection instance.
 *
 * @tags: [
 *   does_not_support_stepdowns,
 *   requires_fcv_83,
 *   uses_atclustertime,
 * ]
 */
import {Thread} from "jstests/libs/parallelTester.js";
import {ReshardingTest} from "jstests/sharding/libs/resharding_test_fixture.js";
import {runJoinsExistingOperationTest} from "jstests/sharding/libs/resharding_test_joins_operation.js";

// Generates a new thread to run rewriteCollection.
const makeRewriteCollectionThread = (mongosHost, ns) => {
    return new Thread(
        (mongosHost, ns) => {
            const mongoS = new Mongo(mongosHost);
            assert.commandWorked(mongoS.adminCommand({rewriteCollection: ns}));
        },
        mongosHost,
        ns,
    );
};

// Use default ReshardingTest (2 shards: 1 donor + 1 recipient) to ensure enough cardinality
// for the resharding coordinator to create chunks without hitting sampling issues.
const reshardingTest = new ReshardingTest();
reshardingTest.setup();
const donorShardNames = reshardingTest.donorShardNames;

// Create a sharded collection - rewriteCollection requires a sharded collection as input.
// Use {oldKey: 1} as the shard key to match the pattern of other resharding tests.
const sourceCollection = reshardingTest.createShardedCollection({
    ns: "reshardingDb.coll",
    shardKeyPattern: {oldKey: 1},
    chunks: [{min: {oldKey: MinKey}, max: {oldKey: MaxKey}, shard: donorShardNames[0]}],
});

// Insert documents with sufficient cardinality in the shard key field.
// The rewriteCollection command uses resharding which by default tries to create ~90 chunks.
// Without enough unique shard key values, it fails with a cardinality error.
const bulkOp = sourceCollection.initializeUnorderedBulkOp();
for (let i = -500; i < 500; ++i) {
    bulkOp.insert({oldKey: i});
}
assert.commandWorked(bulkOp.execute());

runJoinsExistingOperationTest(reshardingTest, {
    opType: "rewriteCollection",
    operationArgs: {
        newChunks: [{min: {oldKey: MinKey}, max: {oldKey: MaxKey}, shard: donorShardNames[0]}],
    },
    makeJoiningThreadFn: makeRewriteCollectionThread,
});

reshardingTest.teardown();
