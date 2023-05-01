/**
 * Tests that the analyzeShardKey command returns monotonicity metrics for compound shard keys that
 * use hashed sharding.
 *
 * @tags: [requires_fcv_70, resource_intensive]
 */
(function() {
"use strict";

load("jstests/sharding/analyze_shard_key/libs/monotonicity_common.js");

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

// This test requires the collection to contain at least a few thousands of documents to smooth out
// the noise in the insertion order caused by the oplog application batching on secondaries.
const numDocsRange = {
    min: 7500,
    max: 10000
};

const setParameterOpts = {
    // Skip calculating the read and write distribution metrics since there are no sampled queries
    // anyway.
    "failpoint.analyzeShardKeySkipCalcalutingReadWriteDistributionMetrics":
        tojson({mode: "alwaysOn"})
};

{
    const st = new ShardingTest({shards: 2, rs: {nodes: 2, setParameter: setParameterOpts}});

    testAnalyzeShardKeysUnshardedCollection(st.s, testCases, numDocsRange);
    testAnalyzeShardKeysShardedCollection(st, testCases, numDocsRange);

    st.stop();
}

{
    const rst = new ReplSetTest({nodes: 2, nodeOptions: {setParameter: setParameterOpts}});
    rst.startSet();
    rst.initiate();
    const primary = rst.getPrimary();

    testAnalyzeShardKeysUnshardedCollection(primary, testCases, numDocsRange);

    rst.stopSet();
}
})();
