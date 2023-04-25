/**
 * Tests support for the configureQueryAnalyzer command.
 *
 * @tags: [requires_fcv_70]
 */
(function() {
"use strict";

// Set this to opt into the sampleRate check.
TestData.testingDiagnosticsEnabled = false;

const dbNameBase = "testDb";

function testNonExistingCollection(testCases, tenantId) {
    const dbName = tenantId ? (tenantId + "-" + dbNameBase) : dbNameBase;
    const collName = "testCollNonExisting";
    const ns = dbName + "." + collName;

    testCases.forEach(testCase => {
        jsTest.log(`Running configureQueryAnalyzer command against an non-existing collection: ${
            tojson({testCase, ns})}`);
        const cmdObj = {configureQueryAnalyzer: ns, mode: "full", sampleRate: 1};
        if (tenantId) {
            cmdObj.$tenant = tenantId;
        }
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

        // Can set 'sampleRate' to > 0.
        const basicRes =
            testCase.conn.adminCommand({configureQueryAnalyzer: ns, mode: "full", sampleRate: 0.1});
        if (testCase.expectedErrorCode) {
            assert.commandFailedWithCode(basicRes, testCase.expectedErrorCode);
            // There is no need to test the remaining cases.
            return;
        }
        assert.commandWorked(basicRes);
        assert.commandWorked(
            testCase.conn.adminCommand({configureQueryAnalyzer: ns, mode: "full", sampleRate: 1}));
        assert.commandWorked(
            testCase.conn.adminCommand({configureQueryAnalyzer: ns, mode: "full", sampleRate: 50}));

        // Cannot set 'sampleRate' to 0.
        assert.commandFailedWithCode(
            testCase.conn.adminCommand({configureQueryAnalyzer: ns, mode: "full", sampleRate: 0}),
            ErrorCodes.InvalidOptions);

        // Cannot set 'sampleRate' to larger than 50.
        assert.commandFailedWithCode(
            testCase.conn.adminCommand({configureQueryAnalyzer: ns, mode: "full", sampleRate: 51}),
            ErrorCodes.InvalidOptions);

        // Cannot specify 'sampleRate' when 'mode' is "off".
        assert.commandFailedWithCode(
            testCase.conn.adminCommand({configureQueryAnalyzer: ns, mode: "off", sampleRate: 1}),
            ErrorCodes.InvalidOptions);
        assert.commandWorked(testCase.conn.adminCommand({configureQueryAnalyzer: ns, mode: "off"}));

        // Cannot specify read/write concern.
        assert.commandFailedWithCode(testCase.conn.adminCommand({
            configureQueryAnalyzer: ns,
            mode: "full",
            sampleRate: 1,
            readConcern: {level: "available"}
        }),
                                     ErrorCodes.InvalidOptions);
        assert.commandFailedWithCode(testCase.conn.adminCommand({
            configureQueryAnalyzer: ns,
            mode: "full",
            sampleRate: 1,
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
    const tenantId = ObjectId();

    // Prepare a user for testing multitenancy via $tenant.
    // Must be authenticated as a user with ActionType::useTenant in order to use $tenant.
    assert.commandWorked(
        adminDb.runCommand({createUser: "admin", pwd: "pwd", roles: ["__system"]}));
    assert(adminDb.auth("admin", "pwd"));

    // The configureQueryAnalyzer command is not supported even on primary mongod.
    const testCases = [];
    testCases.push(Object.assign(
        {conn: primary, isSupported: false, expectedErrorCode: ErrorCodes.IllegalOperation}));
    testNonExistingCollection(testCases, tenantId);

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
})();
