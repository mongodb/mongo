/**
 * Tests that the analyzeShardKey command returns monotonicity metrics for non-compound shard keys
 * that use range sharding.
 *
 * @tags: [requires_fcv_63, featureFlagAnalyzeShardKey]
 */
(function() {
"use strict";

load("jstests/sharding/analyze_shard_key/libs/monotonicity_common.js");

const testCases = [];

// Construct test cases for testing the monotonicity of non-compound shard keys that use range
// sharding. For each test case:
// - 'shardKey' is the shard key being analyzed.
// - 'indexKey' is the supporting index for the shard key.
// - 'fieldOpts' specifies the name, type and order for each field inside the documents in the
//   collection. The order refers to whether the value is constant, fluctuating, increasing or
//   decreasing.
// - 'expected' is the expected monotonicity.
for (let orderType0 of kOrderTypes) {
    const fieldName0 = AnalyzeShardKeyUtil.getRandomFieldName("a");
    const fieldType0 = AnalyzeShardKeyUtil.getRandomElement(orderType0.supportedFieldTypes);

    // Test non-compound shard key with a shard key index.
    testCases.push({
        shardKey: {[fieldName0]: 1},
        indexKey: {[fieldName0]: 1},
        fieldOpts: [{name: fieldName0, type: fieldType0, order: orderType0.name}],
        expected: orderType0.monotonicity
    });

    for (let orderType1 of kOrderTypes) {
        const fieldName1 = AnalyzeShardKeyUtil.getRandomFieldName("b");
        const fieldType1 = AnalyzeShardKeyUtil.getRandomElement(orderType1.supportedFieldTypes);

        // Test non-compound shard key with a compound shard key prefixed index.
        testCases.push({
            shardKey: {[fieldName0]: 1},
            indexKey: {[fieldName0]: 1, [fieldName1]: 1},
            fieldOpts: [
                {name: fieldName0, type: fieldType0, order: orderType0.name},
                {name: fieldName1, type: fieldType1, order: orderType1.name}
            ],
            expected: orderType0.monotonicity
        });
    }
}

// This test requires the collection to contain at least a few thousands of documents to smooth out
// the noise in the insertion order caused by the oplog application batching on secondaries.
const numDocsRange = {
    min: 7500,
    max: 10000
};

{
    const st = new ShardingTest({
        shards: 2,
        rs: {
            nodes: 2,
            setParameter: {
                "failpoint.analyzeShardKeySkipCalcalutingReadWriteDistributionMetrics":
                    tojson({mode: "alwaysOn"})
            }
        }
    });

    testAnalyzeShardKeysUnshardedCollection(st.s, testCases, numDocsRange);
    testAnalyzeShardKeysShardedCollection(st, testCases, numDocsRange);

    st.stop();
}

{
    const rst = new ReplSetTest({nodes: 2});
    rst.startSet();
    rst.initiate();
    const primary = rst.getPrimary();

    testAnalyzeShardKeysUnshardedCollection(primary, testCases, numDocsRange);

    rst.stopSet();
}
})();
