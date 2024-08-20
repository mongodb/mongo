/**
 * Tests support for the configureQueryAnalyzer command.
 *
 * @tags: [requires_fcv_70]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

// This test requires running commands directly against the shard.
TestData.replicaSetEndpointIncompatible = true;

// Set this to opt into the 'samplesPerSecond' check.
TestData.testingDiagnosticsEnabled = false;

const dbNameBase = "testDb";

function testNonExistingCollection(testCases, dbName = dbNameBase) {
    const collName = "testCollNonExisting";
    const ns = dbName + "." + collName;

    testCases.forEach(testCase => {
        jsTest.log(`Running configureQueryAnalyzer command against an non-existing collection: ${
            tojson({testCase, ns})}`);
        const cmdObj = {configureQueryAnalyzer: ns, mode: "full", samplesPerSecond: 1};
        const res = testCase.conn.adminCommand(cmdObj);
        const expectedErrorCode =
            testCase.expectedErrorCode ? testCase.expectedErrorCode : ErrorCodes.NamespaceNotFound;
        assert.commandFailedWithCode(res, expectedErrorCode);
    });
}

function testExistingCollection(writeConn, testCases) {
    const dbName = dbNameBase;
    const collName = "testCollUnsharded";
    const ns = dbName + "." + collName;
    const db = writeConn.getDB(dbName);
    assert.commandWorked(db.createCollection(collName));

    testCases.forEach(testCase => {
        if (testCase.conn.isConfigsvr) {
            // The collection created below will not exist on the config server.
            return;
        }

        jsTest.log(
            `Running configureQueryAnalyzer command against an existing collection:
        ${tojson(testCase)}`);

        // Can set 'samplesPerSecond' to > 0.
        const basicRes = testCase.conn.adminCommand(
            {configureQueryAnalyzer: ns, mode: "full", samplesPerSecond: 0.1});
        if (testCase.expectedErrorCode) {
            assert.commandFailedWithCode(basicRes, testCase.expectedErrorCode);
            // There is no need to test the remaining cases.
            return;
        }
        assert.commandWorked(basicRes);
        assert.commandWorked(testCase.conn.adminCommand(
            {configureQueryAnalyzer: ns, mode: "full", samplesPerSecond: 1}));
        assert.commandWorked(testCase.conn.adminCommand(
            {configureQueryAnalyzer: ns, mode: "full", samplesPerSecond: 50}));

        // Cannot set 'samplesPerSecond' to 0.
        assert.commandFailedWithCode(
            testCase.conn.adminCommand(
                {configureQueryAnalyzer: ns, mode: "full", samplesPerSecond: 0}),
            ErrorCodes.InvalidOptions);

        // Cannot set 'samplesPerSecond' to larger than 50.
        assert.commandFailedWithCode(
            testCase.conn.adminCommand(
                {configureQueryAnalyzer: ns, mode: "full", samplesPerSecond: 51}),
            ErrorCodes.InvalidOptions);

        // Cannot specify 'samplesPerSecond' when 'mode' is "off".
        assert.commandFailedWithCode(
            testCase.conn.adminCommand(
                {configureQueryAnalyzer: ns, mode: "off", samplesPerSecond: 1}),
            ErrorCodes.InvalidOptions);
        assert.commandWorked(testCase.conn.adminCommand({configureQueryAnalyzer: ns, mode: "off"}));

        // Cannot specify read/write concern.
        assert.commandFailedWithCode(testCase.conn.adminCommand({
            configureQueryAnalyzer: ns,
            mode: "full",
            samplesPerSecond: 1,
            readConcern: {level: "available"}
        }),
                                     ErrorCodes.InvalidOptions);
        assert.commandFailedWithCode(testCase.conn.adminCommand({
            configureQueryAnalyzer: ns,
            mode: "full",
            samplesPerSecond: 1,
            writeConcern: {w: "majority"}
        }),
                                     ErrorCodes.InvalidOptions);
    });
}

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

    testNonExistingCollection(testCases);
    testExistingCollection(st.s, testCases);

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

    testNonExistingCollection(testCases);
    testExistingCollection(primary, testCases);

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
    testNonExistingCollection(testCases, "admin");

    rst.stopSet();
}

{
    const mongod = MongoRunner.runMongod();

    // The configureQueryAnalyzer command is not supported on standalone mongod.
    const testCases =
        [{conn: mongod, isSupported: false, expectedErrorCode: ErrorCodes.IllegalOperation}];
    testNonExistingCollection(testCases);

    MongoRunner.stopMongod(mongod);
}
