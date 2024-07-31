import {
    AnalyzeShardKeyUtil
} from "jstests/sharding/analyze_shard_key/libs/analyze_shard_key_util.js";
import {
    kOrderTypes,
} from "jstests/sharding/analyze_shard_key/libs/monotonicity_common.js";

// Construct test cases for testing the monotonicity of compound shard keys that use range sharding.
// For each test case:
// - 'shardKey' is the shard key being analyzed.
// - 'indexKey' is the supporting index for the shard key.
// - 'fieldOpts' specifies the name, type and order for each field inside the documents in the
//   collection. The order refers to whether the value is constant, fluctuating, increasing or
//   decreasing.
// - 'expected' is the expected monotonicity. Since the shard key is compound, its monotonicity is
//   determined by the monotonicity of the first non-constant shard key field.
export const rangeShardingCompoundTestCases = (() => {
    const testCases = [];

    for (let orderType0 of kOrderTypes) {
        const fieldName0 = AnalyzeShardKeyUtil.getRandomFieldName("a");
        const fieldType0 = AnalyzeShardKeyUtil.getRandomElement(orderType0.supportedFieldTypes);

        for (let orderType1 of kOrderTypes) {
            const fieldName1 = AnalyzeShardKeyUtil.getRandomFieldName("b");
            const fieldType1 = AnalyzeShardKeyUtil.getRandomElement(orderType1.supportedFieldTypes);

            // Test compound shard key with a shard key index.
            testCases.push({
                shardKey: {[fieldName0]: 1, [fieldName1]: 1},
                indexKey: {[fieldName0]: 1, [fieldName1]: 1},
                fieldOpts: [
                    {name: fieldName0, type: fieldType0, order: orderType0.name},
                    {name: fieldName1, type: fieldType1, order: orderType1.name}
                ],
                expected: orderType0.name == "constant" ? orderType1.monotonicity
                                                        : orderType0.monotonicity
            });

            for (let orderType2 of kOrderTypes) {
                const fieldName2 = AnalyzeShardKeyUtil.getRandomFieldName("c");
                const fieldType2 =
                    AnalyzeShardKeyUtil.getRandomElement(orderType2.supportedFieldTypes);

                // Test compound shard key with a compound shard key prefixed index.
                testCases.push({
                    shardKey: {[fieldName0]: 1, [fieldName1]: 1},
                    indexKey: {[fieldName0]: 1, [fieldName1]: 1, [fieldName2]: 1},
                    fieldOpts: [
                        {name: fieldName0, type: fieldType0, order: orderType0.name},
                        {name: fieldName1, type: fieldType1, order: orderType1.name},
                        {name: fieldName2, type: fieldType2, order: orderType2.name}
                    ],
                    expected: orderType0.name == "constant" ? orderType1.monotonicity
                                                            : orderType0.monotonicity
                });
            }
        }
    }

    return testCases;
})();
// Need testProbability to cut down on test duration, but still have good coverage in aggregate due
// to number of times tests is ran.
export const testProbability = 0.15;
// This test requires the collection to contain a large number of documents to smooth
// out the insertion order noise caused by parallel oplog application on secondaries.
export const numDocsRange = {
    min: 45000,
    max: 50000
};
