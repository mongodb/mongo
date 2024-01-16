/**
 * Checks that the API version metrics are properly stored and returned.
 *
 * @tags: [
 * ]
 */

function escapeAppName(appName) {
    return escape(appName).replaceAll(/%\d\d/g, (match) => `\\u00${match.substring(1)}`);
}

function runTestWithAppName(uri, appName) {
    const apiVersionsFieldName = "apiVersions";
    const defaultAppName = "MongoDB Shell";
    const testDB = new Mongo(uri + `?appName=${appName}`).getDB(jsTestName());
    const escapedAppName = escapeAppName(appName);

    jsTestLog("Issuing cmd with no API version");
    assert.commandWorked(testDB.runCommand({ping: 1}));

    let serverStatus = testDB.serverStatus().metrics;
    assert(serverStatus.hasOwnProperty(apiVersionsFieldName),
           () => `serverStatus should have an '${apiVersionsFieldName}' field: ${serverStatus}`);

    let apiVersionMetrics = serverStatus[apiVersionsFieldName][escapedAppName];
    assert.eq(["default"], apiVersionMetrics);

    jsTestLog("Issuing cmd with API version 1");
    assert.commandWorked(testDB.runCommand({ping: 1, apiVersion: "1"}));

    serverStatus = testDB.serverStatus().metrics;
    assert(serverStatus.hasOwnProperty(apiVersionsFieldName),
           () => `serverStatus should have an '${apiVersionsFieldName}' field: ${serverStatus}`);

    apiVersionMetrics = serverStatus[apiVersionsFieldName][escapedAppName];
    assert.eq(["default", "1"], apiVersionMetrics);

    const testDBDefaultAppName = conn.getDB(jsTestName());

    jsTestLog("Issuing cmd with default app name");
    assert.commandWorked(testDBDefaultAppName.runCommand({ping: 1}));

    serverStatus = testDB.serverStatus().metrics;
    assert(serverStatus.hasOwnProperty(apiVersionsFieldName),
           () => `serverStatus should have an '${apiVersionsFieldName}' field: ${serverStatus}`);
    assert(serverStatus[apiVersionsFieldName].hasOwnProperty(escapedAppName),
           () => `serverStatus should store metrics for '${escapedAppName}': ${serverStatus}`);
    assert(serverStatus[apiVersionsFieldName].hasOwnProperty(defaultAppName),
           () => `serverStatus should store metrics for '${defaultAppName}': ${serverStatus}`);
}

const conn = MongoRunner.runMongod();
const uri = "mongodb://" + conn.host + "/test";

const appNames = [
    "apiVersionMetricsTest",
    "null\0\0\0\0",
];
for (const appName of appNames) {
    runTestWithAppName(uri, appName);
}

MongoRunner.stopMongod(conn);
