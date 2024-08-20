/**
 * Tests that the analyzeShardKey command returns monotonicity metrics for compound shard keys that
 * use hashed sharding.
 *
 * @tags: [requires_fcv_70, resource_intensive]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {
    AnalyzeShardKeyUtil
} from "jstests/sharding/analyze_shard_key/libs/analyze_shard_key_util.js";
import {
    kOrderTypes,
    testAnalyzeShardKeysShardedCollection,
    testAnalyzeShardKeysUnshardedCollection,
} from "jstests/sharding/analyze_shard_key/libs/monotonicity_common.js";
import {numNodesPerRS} from "jstests/sharding/analyze_shard_key/libs/monotonicity_common.js";

// Construct test cases for testing the monotonicity of compound shard keys that use hashed
// sharding. For each test case:
// - 'shardKey' is the shard key being analyzed.
// - 'indexKey' is the supporting index for the shard key.
// - 'fieldOpts' specifies the name, type and order for each field inside the documents in the
//   collection. The order refers to whether the value is constant, fluctuating, increasing or
//   decreasing.
// - 'expected' is the expected monotonicity. Since the shard key is compound, its monotonicity is
//   determined by the monotonicity of the first non-constant shard key field. However, the
//   monotonicity of a hashed shard key cannot inferred from the recordIds in the index since
//   hashing introduces randomness. So the analyzeShardKey command handles hashed shard keys as
//   follows. If the first field is hashed, it returns "not monotonic". Otherwise, it returns
//   "unknown".
const testCases = [];

for (let orderType0 of kOrderTypes) {
    const fieldName0 = AnalyzeShardKeyUtil.getRandomFieldName("a");
    const fieldType0 = AnalyzeShardKeyUtil.getRandomElement(orderType0.supportedFieldTypes);

    for (let orderType1 of kOrderTypes) {
        const fieldName1 = AnalyzeShardKeyUtil.getRandomFieldName("b");
        const fieldType1 = AnalyzeShardKeyUtil.getRandomElement(orderType1.supportedFieldTypes);

        // Test compound shard key with a hashed prefix.
        testCases.push({
            shardKey: {[fieldName0]: "hashed", [fieldName1]: 1},
            indexKey: {[fieldName0]: "hashed", [fieldName1]: 1},
            fieldOpts: [
                {name: fieldName0, type: fieldType0, order: orderType0.name},
                {name: fieldName1, type: fieldType1, order: orderType1.name}
            ],
            expected: "not monotonic"
        });

        // Test compound shard key without a hashed prefix.
        testCases.push({
            shardKey: {[fieldName0]: 1, [fieldName1]: "hashed"},
            indexKey: {[fieldName0]: 1, [fieldName1]: "hashed"},
            fieldOpts: [
                {name: fieldName0, type: fieldType0, order: orderType0.name},
                {name: fieldName1, type: fieldType1, order: orderType1.name}
            ],
            expected: "unknown"
        });
    }
}
// Need testProbability to cut down on test duration, but still have good coverage in aggregate due
// to number of times tests is ran.
const testProbability = 0.2;
// This test requires the collection to contain at least a few thousands of documents to smooth out
// the insertion order noise caused by parallel oplog application on secondaries.
const numDocsRange = {
    min: 7500,
    max: 10000
};

{
    const st = new ShardingTest({shards: 2, rs: {nodes: numNodesPerRS}});

    testAnalyzeShardKeysUnshardedCollection(st.s, testCases, testProbability, numDocsRange);
    testAnalyzeShardKeysShardedCollection(st, testCases, testProbability, numDocsRange);

    st.stop();
}

if (!jsTestOptions().useAutoBootstrapProcedure) {  // TODO: SERVER-80318 Remove block
    const rst = new ReplSetTest({nodes: numNodesPerRS});
    rst.startSet();
    rst.initiate();
    const primary = rst.getPrimary();

    testAnalyzeShardKeysUnshardedCollection(primary, testCases, testProbability, numDocsRange);

    rst.stopSet();
}
