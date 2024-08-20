/**
 * Tests that basic validation within the configureQueryAnalyzer command.
 *
 * @tags: [requires_fcv_70]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {ValidationTest} from "jstests/sharding/analyze_shard_key/libs/validation_common.js";

function runTest(conn) {
    const validationTest = ValidationTest(conn);
    for (let {dbName, collName, isView} of validationTest.invalidNamespaceTestCases) {
        jsTest.log(
            `Testing that the configureQueryAnalyzer command fails if the namespace is invalid ${
                tojson({dbName, collName})}`);
        const aggCmdObj = {
            configureQueryAnalyzer: dbName + "." + collName,
            mode: "full",
            samplesPerSecond: 1
        };
        assert.commandFailedWithCode(
            conn.adminCommand(aggCmdObj),
            isView ? ErrorCodes.CommandNotSupportedOnView : ErrorCodes.IllegalOperation);
    }
}

{
    const st = new ShardingTest({shards: 1});
    runTest(st.s);
    st.stop();
}

if (!jsTestOptions().useAutoBootstrapProcedure) {  // TODO: SERVER-80318 Remove block
    const rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();
    const primary = rst.getPrimary();
    runTest(primary);
    rst.stopSet();
}