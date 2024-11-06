/**
 * Defines common functions for testing the configureQueryAnalyzer command.
 */

/**
 * Tests that the configureQueryAnalyzer command does not work for a non-existing collection.
 */
export function testNonExistingCollection(testCases, dbName) {
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

/**
 * Tests the configureQueryAnalyzer command with with various options for an existing collection.
 */
export function testExistingCollection(writeConn, testCases, dbName) {
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

        // This check only applies when testingDiagnosticsEnabled is false. However for core tests,
        // this parameter is always true and cannot be modified (the parameter is not runtime
        // modifiable and core test server fixtures cannot be restarted).
        if (!testCase.isCoreTest) {
            // Cannot set 'samplesPerSecond' to larger than 50.
            assert.commandFailedWithCode(
                testCase.conn.adminCommand(
                    {configureQueryAnalyzer: ns, mode: "full", samplesPerSecond: 51}),
                ErrorCodes.InvalidOptions);
        }

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
