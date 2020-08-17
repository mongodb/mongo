/**
 * Checks that the API version metrics are properly stored and returned.
 *
 * @tags: [requires_fcv_47]
 */

(function() {
"use strict";

const apiVersionsFieldName = "apiVersions";
const appName = "apiVersionMetricsTest";
const defaultAppName = "MongoDB Shell";

const conn = MongoRunner.runMongod();
const uri = "mongodb://" + conn.host + "/test";

const testDB = new Mongo(uri + `?appName=${appName}`).getDB(jsTestName());

jsTestLog("Issuing cmd with no API version");
assert.commandWorked(testDB.runCommand({ping: 1}));

let serverStatus = testDB.serverStatus().metrics;
assert(serverStatus.hasOwnProperty(apiVersionsFieldName),
       () => `serverStatus should have an '${apiVersionsFieldName}' field: ${serverStatus}`);

let apiVersionMetrics = serverStatus[apiVersionsFieldName][appName];
assert.eq(["default"], apiVersionMetrics);

jsTestLog("Issuing cmd with API version 1");
assert.commandWorked(testDB.runCommand({ping: 1, apiVersion: "1"}));

serverStatus = testDB.serverStatus().metrics;
assert(serverStatus.hasOwnProperty(apiVersionsFieldName),
       () => `serverStatus should have an '${apiVersionsFieldName}' field: ${serverStatus}`);

apiVersionMetrics = serverStatus[apiVersionsFieldName][appName];
assert.eq(["default", "1"], apiVersionMetrics);

const testDBDefaultAppName = conn.getDB(jsTestName());

jsTestLog("Issuing cmd with default app name");
assert.commandWorked(testDBDefaultAppName.runCommand({ping: 1}));

serverStatus = testDB.serverStatus().metrics;
assert(serverStatus.hasOwnProperty(apiVersionsFieldName),
       () => `serverStatus should have an '${apiVersionsFieldName}' field: ${serverStatus}`);
assert(serverStatus[apiVersionsFieldName].hasOwnProperty(appName),
       () => `serverStatus should store metrics for '${appName}': ${serverStatus}`);
assert(serverStatus[apiVersionsFieldName].hasOwnProperty(defaultAppName),
       () => `serverStatus should store metrics for '${defaultAppName}': ${serverStatus}`);

MongoRunner.stopMongod(conn);
})();
