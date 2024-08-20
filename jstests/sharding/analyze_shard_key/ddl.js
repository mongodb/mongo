/**
 * Tests that the analyzeShardKey command and the configureQueryAnalyzer command fail with expected
 * errors if the collection is dropped, renamed, recreated or emptied while they are running.
 *
 * @tags: [requires_fcv_70]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";
import {QuerySamplingUtil} from "jstests/sharding/analyze_shard_key/libs/query_sampling_util.js";
import {ValidationTest} from "jstests/sharding/analyze_shard_key/libs/validation_common.js";

const queryAnalysisSamplerConfigurationRefreshSecs = 1;
const analyzeShardKeyNumRanges = 10;
// The sampling-based initial split policy needs 10 samples per split point so
// 10 * analyzeShardKeyNumRanges is the minimum number of distinct shard key values that the
// collection must have for the command to not fail to generate split points (i.e. fail with the
// error code 4952606).
const numDocs = 10 * analyzeShardKeyNumRanges;

// Given the number of documents defined above, the error code 4952606 is only expected because of
// the deletes that will occur as part of renaming, dropping, recreating and emptying the
// collection.
const expectedAnalyzeShardKeyErrCodes = [
    ErrorCodes.NamespaceNotFound,
    ErrorCodes.QueryPlanKilled,
    ErrorCodes.IllegalOperation,
    // The shard key does not have enough cardinality for generating split points because
    // documents are being deleted.
    4952606,
    // The collection becomes empty during the $collStats step.
    7826501,
    // The collection becomes empty during the step for calculating the monotonicity metrics.
    7826505,
    // The collection becomes empty during the step for calculating the cardinality and frequency
    // metrics.
    7826506
];
const analyzeShardKeyTestCases = [
    {operationType: "rename", expectedErrCodes: expectedAnalyzeShardKeyErrCodes},
    {operationType: "drop", expectedErrCodes: expectedAnalyzeShardKeyErrCodes},
    {
        operationType: "recreate",
        expectedErrCodes:
            [ErrorCodes.CollectionUUIDMismatch].concat(expectedAnalyzeShardKeyErrCodes)
    },
    {operationType: "makeEmpty", expectedErrCodes: expectedAnalyzeShardKeyErrCodes}
];
// Test DDL operations after each step below.
const analyzeShardKeyFpNames = [
    "analyzeShardKeyPauseBeforeCalculatingKeyCharacteristicsMetrics",
    "analyzeShardKeyPauseBeforeCalculatingReadWriteDistributionMetrics"
];

const configureQueryAnalyzerTestCases = [
    {operationType: "rename", expectedErrCodes: [ErrorCodes.NamespaceNotFound]},
    {operationType: "drop", expectedErrCodes: [ErrorCodes.NamespaceNotFound]},
    {operationType: "recreate", expectedErrCodes: [ErrorCodes.NamespaceNotFound]},
    {operationType: "makeEmpty", expectedErrCodes: [ErrorCodes.IllegalOperation]}
];

function setUpTestCase(conn, dbName, collName, operationType) {
    const testDB = conn.getDB(dbName);
    const testColl = testDB.getCollection(collName);
    switch (operationType) {
        case "rename":
            assert.commandWorked(testColl.renameCollection(extractUUIDFromObject(UUID())));
            break;
        case "drop":
            assert(testColl.drop());
            break;
        case "recreate":
            assert(testColl.drop());
            assert.commandWorked(testDB.createCollection(collName));
            break;
        case "makeEmpty":
            assert.commandWorked(testColl.remove({}));
            break;
        default:
            new Error("Unknown test case");
    }
}

function runAnalyzeShardKeyTest(conn, testCase, fpConn, fpName) {
    const validationTest = ValidationTest(conn);

    const dbName = validationTest.dbName;
    const collName = validationTest.collName;
    const ns = dbName + "." + collName;
    jsTest.log(`Testing analyzeShardKey command ${tojson({testCase, dbName, collName, fpName})}`);
    const {docs} = validationTest.makeDocuments(numDocs);
    assert.commandWorked(conn.getCollection(ns).insert(docs));

    const runCmdFunc = async (host, ns) => {
        const {AnalyzeShardKeyUtil} =
            await import("jstests/sharding/analyze_shard_key/libs/analyze_shard_key_util.js");
        const conn = new Mongo(host);
        sleep(AnalyzeShardKeyUtil.getRandInteger(10, 100));
        return conn.adminCommand({analyzeShardKey: ns, key: {_id: 1}});
    };

    let runCmdThread = new Thread(runCmdFunc, conn.host, ns);
    let fp;
    if (fpName) {
        fp = configureFailPoint(fpConn, fpName);
    }
    runCmdThread.start();
    setUpTestCase(conn, dbName, collName, testCase.operationType);
    if (fp) {
        fp.off();
    }
    assert.commandWorkedOrFailedWithCode(runCmdThread.returnData(), testCase.expectedErrCodes);
}

async function runConfigureQueryAnalyzerTest(conn, testCase, {rst} = {}) {
    const validationTest = ValidationTest(conn);

    const dbName = validationTest.dbName;
    const collName = validationTest.collName;
    const ns = dbName + "." + collName;

    jsTest.log(`Testing configureQueryAnalyzer command ${tojson({testCase, dbName, collName})}`);

    const runCmdFunc = async (host, ns, mode, samplesPerSecond) => {
        const {AnalyzeShardKeyUtil} =
            await import("jstests/sharding/analyze_shard_key/libs/analyze_shard_key_util.js");
        const conn = new Mongo(host);
        sleep(AnalyzeShardKeyUtil.getRandInteger(10, 100));
        return conn.adminCommand({configureQueryAnalyzer: ns, mode, samplesPerSecond});
    };

    let runCmdThread =
        new Thread(runCmdFunc, conn.host, ns, "full" /* mode */, 1 /* samplesPerSecond */);
    runCmdThread.start();
    setUpTestCase(conn, dbName, collName, testCase.operationType);
    const res =
        assert.commandWorkedOrFailedWithCode(runCmdThread.returnData(), testCase.expectedErrCodes);

    if (testCase.operationType == "recreate") {
        const configDoc = conn.getCollection("config.queryAnalyzers").findOne({_id: ns});
        if (configDoc) {
            // The configureQueryAnalyzer command is serialized with DDL commands. In addition, on a
            // sharded cluster, dropping a collection causes its config.queryAnalyzers document to
            // get deleted. So if there is config.queryAnalyzers document for this collection after
            // it is dropped and recreated, the configureQueryAnalyzer command must have run after
            // the collection has been recreated so it must have the new collection uuid.
            assert(res.ok, res);
            const collUuid = QuerySamplingUtil.getCollectionUuid(conn.getDB(dbName), collName);
            if (bsonWoCompare(configDoc.collUuid, collUuid) != 0) {
                // (SERVER-76443): Make sure that dropCollection on replica set delete the
                // config.queryAnalyzers doc for the collection being dropped.
                assert(rst);
            }
        }
    }

    // Verify that running the configureQueryAnalyzer command after the DDL operation does not
    // lead to a crash.
    sleep(queryAnalysisSamplerConfigurationRefreshSecs);
    assert.commandWorkedOrFailedWithCode(
        await runCmdFunc(conn.host, ns, "full" /* mode */, 10 /* samplesPerSecond */),
        testCase.expectedErrCodes);
    sleep(queryAnalysisSamplerConfigurationRefreshSecs);
    assert.commandWorkedOrFailedWithCode(await runCmdFunc(conn.host, ns, "off" /* mode */),
                                         testCase.expectedErrCodes);
}

{
    const st = new ShardingTest({
        shards: 1,
        rs: {
            nodes: 1,
            setParameter: {
                queryAnalysisSamplerConfigurationRefreshSecs,
                analyzeShardKeyNumRanges,
                logComponentVerbosity: tojson({sharding: 2})
            }
        },
    });
    const shard0Primary = st.rs0.getPrimary();

    for (let testCase of analyzeShardKeyTestCases) {
        runAnalyzeShardKeyTest(st.s, testCase);
        for (let fpName of analyzeShardKeyFpNames) {
            runAnalyzeShardKeyTest(st.s, testCase, shard0Primary, fpName);
        }
    }
    for (let testCase of configureQueryAnalyzerTestCases) {
        await runConfigureQueryAnalyzerTest(st.s, testCase);
    }

    st.stop();
}

if (!jsTestOptions().useAutoBootstrapProcedure) {  // TODO: SERVER-80318 Remove block
    const rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();
    const primary = rst.getPrimary();

    for (let testCase of analyzeShardKeyTestCases) {
        runAnalyzeShardKeyTest(primary, testCase);
        for (let fpName of analyzeShardKeyFpNames) {
            runAnalyzeShardKeyTest(primary, testCase, primary, fpName);
        }
    }
    for (let testCase of configureQueryAnalyzerTestCases) {
        await runConfigureQueryAnalyzerTest(primary, testCase, {rst});
    }

    rst.stopSet();
}
