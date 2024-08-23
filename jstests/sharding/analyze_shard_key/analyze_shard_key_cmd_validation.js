/**
 * Tests that basic validation within the analyzeShardKey command.
 *
 * @tags: [requires_fcv_70]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {ValidationTest} from "jstests/sharding/analyze_shard_key/libs/validation_common.js";

const analyzeShardKeyNumRanges = 10;

function testValidationBeforeMetricsCalculation(conn, mongodConn, validationTest) {
    jsTest.log(`Testing validation before calculating any metrics`);

    // Set the fail point that would make the analyzeShardKey command fail with an InternalError
    // before metrics calculation. That way if there is no expected validation before the metrics
    // calculation, the command would fail with an InternalError error instead of the expected
    // error.
    let fp = configureFailPoint(mongodConn, "analyzeShardKeyFailBeforeMetricsCalculation");

    for (let {dbName, collName, isView} of validationTest.invalidNamespaceTestCases) {
        jsTest.log(`Testing that the analyzeShardKey command fails if the namespace is invalid ${
            tojson({dbName, collName})}`);
        const ns = dbName + "." + collName;
        assert.commandFailedWithCode(
            conn.adminCommand({analyzeShardKey: ns, key: {_id: 1}}),
            isView ? ErrorCodes.CommandNotSupportedOnView : ErrorCodes.IllegalOperation);
    }
    for (let shardKey of validationTest.invalidShardKeyTestCases) {
        jsTest.log(`Testing that the analyzeShardKey command fails if the shard key is invalid ${
            tojson({shardKey})}`);
        const ns = validationTest.validDbName + "." + validationTest.validCollName;
        assert.commandFailedWithCode(conn.adminCommand({analyzeShardKey: ns, key: shardKey}),
                                     ErrorCodes.BadValue);
    }

    fp.off();
}

function testValidationDuringKeyCharacteristicsMetricsCalculation(conn, validationTest) {
    const dbName = validationTest.dbName;
    const collName = validationTest.collName;
    const ns = dbName + "." + collName;
    jsTest.log(
        `Testing validation while calculating metrics about the characteristics of the shard key ${
            tojson(dbName, collName)}`);

    const testDB = conn.getDB(dbName);
    const testColl = testDB.getCollection(collName);

    jsTest.log("Testing that the analyzeShardKey command fails to calculate the metrics if the" +
               " shard key contains an array field");
    const {docs, arrayFieldName} = validationTest.makeDocuments(1);
    assert.commandWorked(testColl.insert(docs));
    assert.commandFailedWithCode(
        conn.adminCommand({analyzeShardKey: ns, key: {[arrayFieldName]: 1}}), ErrorCodes.BadValue);

    jsTest.log("Testing that the analyzeShardKey command doesn't use an index that is not a" +
               " b-tree or hashed index to calculate the metrics");
    // To make the collection non-empty, keep the document from above but set the value of the
    // array field to null (otherwise, the command would fail with a BadValue error again).
    assert.commandWorked(testColl.update({}, {[arrayFieldName]: null}));
    for (let {indexOptions, shardKey} of validationTest.noCompatibleIndexTestCases) {
        jsTest.log(`Testing incompatible index ${tojson({indexOptions, shardKey})}`);
        assert.commandWorked(testDB.runCommand({createIndexes: collName, indexes: [indexOptions]}));
        assert.commandFailedWithCode(conn.adminCommand({
            analyzeShardKey: ns,
            key: shardKey,
            keyCharacteristics: true,
            readWriteDistribution: false
        }),
                                     ErrorCodes.IllegalOperation);
        assert.commandWorked(testDB.runCommand({dropIndexes: collName, index: indexOptions.name}));
    }

    assert.commandWorked(testColl.remove({}));
}

function testValidationDuringReadWriteDistributionMetricsCalculation(
    cmdConn, validationTest, aggConn) {
    const dbName = validationTest.dbName;
    const collName = validationTest.collName;
    const ns = dbName + "." + collName;
    jsTest.log(`Testing validation while calculating metrics about read and write distribution ${
        tojson(dbName, collName)}`);

    const testDB = cmdConn.getDB(dbName);
    const testColl = testDB.getCollection(collName);
    // The sampling-based initial split policy needs 10 samples per split point so
    // 10 * analyzeShardKeyNumRanges is the minimum number of distinct shard key values that the
    // collection must have for the command to not fail to generate split points.
    const {docs, arrayFieldName} = validationTest.makeDocuments(10 * analyzeShardKeyNumRanges);

    let fp = configureFailPoint(
        aggConn, "analyzeShardKeyPauseBeforeCalculatingReadWriteDistributionMetrics");
    let analyzeShardKeyFunc = (cmdHost, ns, arrayFieldName) => {
        const cmdConn = new Mongo(cmdHost);
        return cmdConn.adminCommand({
            analyzeShardKey: ns,
            key: {[arrayFieldName]: 1},
            keyCharacteristics: false,
            readWriteDistribution: true
        });
    };
    let analyzeShardKeyThread = new Thread(analyzeShardKeyFunc, cmdConn.host, ns, arrayFieldName);

    // Insert the documents but set the array field to null so it passes the best-effort validation
    // at the start of the command.
    assert.commandWorked(testColl.insert(docs));
    assert.commandWorked(testColl.update({}, {[arrayFieldName]: null}));
    analyzeShardKeyThread.start();
    fp.wait();

    // Re-insert the documents. The best-effort validation when generating split points should
    // detect that the shard key contains an array field and fail.
    assert.commandWorked(testColl.remove({}));
    assert.commandWorked(testColl.insert(docs));
    fp.off();
    assert.commandFailedWithCode(analyzeShardKeyThread.returnData(), ErrorCodes.BadValue);

    assert.commandWorked(testColl.remove({}));
}

const setParameterOpts = {analyzeShardKeyNumRanges};

{
    const st = new ShardingTest({shards: 1, rs: {nodes: 1, setParameter: setParameterOpts}});
    const shard0Primary = st.rs0.getPrimary();
    const validationTest = ValidationTest(st.s);

    // Disable the calculation of all metrics to test validation at the start of the command.
    testValidationBeforeMetricsCalculation(st.s, shard0Primary, validationTest);
    testValidationDuringKeyCharacteristicsMetricsCalculation(st.s, validationTest);
    testValidationDuringReadWriteDistributionMetricsCalculation(
        st.s, validationTest, shard0Primary);

    st.stop();
}

if (!jsTestOptions().useAutoBootstrapProcedure) {  // TODO: SERVER-80318 Remove block
    const rst = new ReplSetTest({nodes: 2, nodeOptions: {setParameter: setParameterOpts}});
    rst.startSet();
    rst.initiate();
    const primary = rst.getPrimary();

    const validationTest = ValidationTest(primary);

    testValidationBeforeMetricsCalculation(primary, primary, validationTest);
    testValidationDuringKeyCharacteristicsMetricsCalculation(primary, validationTest);
    testValidationDuringReadWriteDistributionMetricsCalculation(primary, validationTest, primary);

    rst.stopSet();
}
