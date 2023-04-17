/**
 * Tests that the analyzeShardKey command and the configureQueryAnalyzer command fail with expected
 * errors if the collection is dropped, renamed, recreated or emptied while they are running.
 *
 * @tags: [requires_fcv_70]
 */
(function() {
"use strict";

load("jstests/libs/config_shard_util.js");
load("jstests/libs/fail_point_util.js");
load("jstests/sharding/analyze_shard_key/libs/analyze_shard_key_util.js");
load("jstests/sharding/analyze_shard_key/libs/validation_common.js");

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
const analyzeShardKeyTestCases = [
    {
        operationType: "rename",
        expectedErrCodes: [ErrorCodes.NamespaceNotFound, ErrorCodes.QueryPlanKilled, 4952606]
    },
    {
        operationType: "drop",
        expectedErrCodes: [ErrorCodes.NamespaceNotFound, ErrorCodes.QueryPlanKilled, 4952606]
    },
    {
        operationType: "recreate",
        expectedErrCodes: [
            ErrorCodes.NamespaceNotFound,
            ErrorCodes.CollectionUUIDMismatch,
            ErrorCodes.QueryPlanKilled,
            ErrorCodes.IllegalOperation,
            4952606
        ]
    },
    {operationType: "makeEmpty", expectedErrCodes: [ErrorCodes.IllegalOperation, 4952606]}
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

function setUpTestMode(conn, dbName, collName, operationType) {
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

    const runCmdFunc = (host, ns) => {
        const conn = new Mongo(host);
        return conn.adminCommand({analyzeShardKey: ns, key: {_id: 1}});
    };

    let runCmdThread = new Thread(runCmdFunc, conn.host, ns);
    let fp;
    if (fpName) {
        fp = configureFailPoint(fpConn, fpName);
    }
    runCmdThread.start();
    sleep(AnalyzeShardKeyUtil.getRandInteger(10, 100));
    setUpTestMode(conn, dbName, collName, testCase.operationType);
    if (fp) {
        fp.off();
    }
    assert.commandWorkedOrFailedWithCode(runCmdThread.returnData(), testCase.expectedErrCodes);
}

function runConfigureQueryAnalyzerTest(conn, testCase) {
    const validationTest = ValidationTest(conn);

    const dbName = validationTest.dbName;
    const collName = validationTest.collName;
    const ns = dbName + "." + collName;
    jsTest.log(`Testing configureQueryAnalyzer command ${tojson({testCase, dbName, collName})}`);

    const runCmdFunc = (host, ns, mode, sampleRate) => {
        const conn = new Mongo(host);
        return conn.adminCommand({configureQueryAnalyzer: ns, mode, sampleRate});
    };

    let runCmdThread = new Thread(runCmdFunc, conn.host, ns, "full" /* mode */, 1 /* sampleRate */);
    runCmdThread.start();
    sleep(AnalyzeShardKeyUtil.getRandInteger(10, 100));
    setUpTestMode(conn, dbName, collName, testCase.operationType);
    assert.commandWorkedOrFailedWithCode(runCmdThread.returnData(), testCase.expectedErrCodes);

    // Verify that running the configureQueryAnalyzer command after the DDL operation does not
    // lead to a crash.
    sleep(queryAnalysisSamplerConfigurationRefreshSecs);
    assert.commandWorkedOrFailedWithCode(
        runCmdFunc(conn.host, ns, "full" /* mode */, 10 /* sampleRate */),
        testCase.expectedErrCodes);
    sleep(queryAnalysisSamplerConfigurationRefreshSecs);
    assert.commandWorkedOrFailedWithCode(runCmdFunc(conn.host, ns, "off" /* mode */),
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
        runConfigureQueryAnalyzerTest(st.s, testCase);
    }

    st.stop();
}

{
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
        runConfigureQueryAnalyzerTest(primary, testCase);
    }

    rst.stopSet();
}
})();
