/**
 * Tests that basic validation within the analyzeShardKey command.
 *
 * @tags: [requires_fcv_70, featureFlagAnalyzeShardKey]
 */
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/sharding/analyze_shard_key/libs/analyze_shard_key_util.js");
load("jstests/sharding/analyze_shard_key/libs/validation_common.js");

function testValidationBeforeMetricsCalculation(conn, validationTest) {
    jsTest.log(`Testing validation before calculating any metrics`);

    for (let {dbName, collName, isView} of validationTest.invalidNamespaceTestCases) {
        jsTest.log(`Testing that the analyzeShardKey command fails if the namespace is invalid ${
            tojson({dbName, collName})}`);
        const ns = dbName + "." + collName;
        assert.commandFailedWithCode(
            conn.adminCommand({analyzeShardKey: ns, key: {_id: 1}}),
            isView ? ErrorCodes.CommandNotSupportedOnView : ErrorCodes.IllegalOperation);
    }
}

{
    const st = new ShardingTest({shards: 1, rs: {nodes: 1}});
    const shard0Primary = st.rs0.getPrimary();
    const validationTest = ValidationTest(st.s);

    // Disable the calculation of all metrics to test validation at the start of the command.
    let fp0 =
        configureFailPoint(shard0Primary, "analyzeShardKeySkipCalcalutingKeyCharactericsMetrics");
    let fp1 = configureFailPoint(shard0Primary,
                                 "analyzeShardKeySkipCalcalutingReadWriteDistributionMetrics");
    testValidationBeforeMetricsCalculation(st.s, validationTest);

    fp0.off();
    fp1.off();
    st.stop();
}

{
    const rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();
    const primary = rst.getPrimary();

    const validationTest = ValidationTest(primary);

    // Disable the calculation of all metrics to test validation at the start of the command.
    let fp0 = configureFailPoint(primary, "analyzeShardKeySkipCalcalutingKeyCharactericsMetrics");
    let fp1 =
        configureFailPoint(primary, "analyzeShardKeySkipCalcalutingReadWriteDistributionMetrics");
    testValidationBeforeMetricsCalculation(primary, validationTest);

    fp0.off();
    fp1.off();
    rst.stopSet();
}
})();
