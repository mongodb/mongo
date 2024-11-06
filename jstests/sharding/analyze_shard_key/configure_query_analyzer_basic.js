/**
 * Tests support for the configureQueryAnalyzer command.
 *
 * @tags: [requires_fcv_70]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {
    testExistingCollection,
    testNonExistingCollection
} from "jstests/sharding/analyze_shard_key/libs/configure_query_analyzer_common.js";

// This test requires running commands directly against the shard.
TestData.replicaSetEndpointIncompatible = true;

// Set this to opt into the 'samplesPerSecond' check.
TestData.testingDiagnosticsEnabled = false;

const dbNameBase = "testDb";

{
    const st = new ShardingTest({shards: 1, rs: {nodes: 2}});

    const shard0Primary = st.rs0.getPrimary();
    const shard0Secondaries = st.rs0.getSecondaries();
    const configPrimary = st.configRS.getPrimary();
    const configSecondaries = st.configRS.getSecondaries();
    st.configRS.nodes.forEach(node => {
        node.isConfigsvr = true;
    });

    const testCases = [];
    // The configureQueryAnalyzer command is only supported on mongos and shardsvr primary mongod.
    testCases.push({conn: st.s});
    testCases.push({
        conn: shard0Primary,
        // It is illegal to send a configureQueryAnalyzer command to a shardsvr mongod without
        // attaching the database version.
        expectedErrorCode: ErrorCodes.IllegalOperation
    });
    shard0Secondaries.forEach(node => {
        testCases.push({
            conn: node,
            // configureQueryAnalyzer is a primary-only command.
            expectedErrorCode: ErrorCodes.NotWritablePrimary
        });
    });
    // The analyzeShardKey command is not supported on dedicated configsvr mongods.
    testCases.push({conn: configPrimary, expectedErrorCode: ErrorCodes.IllegalOperation});
    configSecondaries.forEach(node => {
        testCases.push({
            conn: node,
            // configureQueryAnalyzer is a primary-only command.
            expectedErrorCode: ErrorCodes.NotWritablePrimary
        });
    });

    testNonExistingCollection(testCases, dbNameBase);
    testExistingCollection(st.s, testCases, dbNameBase);

    st.stop();
}

{
    // Verify that an external client cannot run the configureQueryAnalyzer command against a
    // shardsvr mongod.

    // Start a sharded cluster with testing diagnostics (TestingProctor) disabled so the command
    // below not bypass the internal client check.
    TestData.testingDiagnosticsEnabled = false;

    const st = new ShardingTest({shards: 1, rs: {nodes: 1}});
    const shard0Primary = st.rs0.getPrimary();

    const dbName = "testDb";
    const collName = "testColl";
    const ns = dbName + "." + collName;

    assert.commandWorked(st.s.getCollection(ns).insert({x: 1}));

    const configureRes = assert.commandFailedWithCode(
        shard0Primary.adminCommand({configureQueryAnalyzer: ns, mode: "full", samplesPerSecond: 1}),
        ErrorCodes.IllegalOperation);
    // Verify that the error message is as expected.
    assert.eq(configureRes.errmsg,
              "Cannot run configureQueryAnalyzer command directly against a shardsvr mongod");

    st.stop();
}

if (jsTestOptions().useAutoBootstrapProcedure) {  // TODO: SERVER-80318 Remove tests below
    quit();
}

{
    const rst = new ReplSetTest({name: jsTest.name() + "_non_multitenant", nodes: 2});
    rst.startSet();
    rst.initiate();
    const primary = rst.getPrimary();
    const secondaries = rst.getSecondaries();

    const testCases = [];
    // The configureQueryAnalyzer command is only supported on primary mongod.
    testCases.push(Object.assign({conn: primary}));
    secondaries.forEach(node => {
        testCases.push({
            conn: node,
            // configureQueryAnalyzer is a primary-only command.
            expectedErrorCode: ErrorCodes.NotWritablePrimary
        });
    });

    testNonExistingCollection(testCases, dbNameBase);
    testExistingCollection(primary, testCases, dbNameBase);

    rst.stopSet();
}

if (!TestData.auth) {
    const rst = new ReplSetTest({
        name: jsTest.name() + "_multitenant",
        nodes: 2,
        nodeOptions: {setParameter: {multitenancySupport: true}}
    });
    rst.startSet({keyFile: "jstests/libs/key1"});
    rst.initiate();
    const primary = rst.getPrimary();
    const adminDb = primary.getDB("admin");

    // Prepare an authenticated user for testing.
    // Must be authenticated as a user with ActionType::useTenant in order to use security token
    assert.commandWorked(
        adminDb.runCommand({createUser: "admin", pwd: "pwd", roles: ["__system"]}));
    assert(adminDb.auth("admin", "pwd"));

    // The configureQueryAnalyzer command is not supported even on primary mongod.
    const testCases = [];
    testCases.push(Object.assign(
        {conn: primary, isSupported: false, expectedErrorCode: ErrorCodes.IllegalOperation}));
    testNonExistingCollection(testCases, "admin", dbNameBase);

    rst.stopSet();
}

{
    const mongod = MongoRunner.runMongod();

    // The configureQueryAnalyzer command is not supported on standalone mongod.
    const testCases =
        [{conn: mongod, isSupported: false, expectedErrorCode: ErrorCodes.IllegalOperation}];
    testNonExistingCollection(testCases, dbNameBase);

    MongoRunner.stopMongod(mongod);
}
