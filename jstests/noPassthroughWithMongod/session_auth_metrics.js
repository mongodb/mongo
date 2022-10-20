/**
 * Tests for the connection establishment metrics.
 *
 * @tags: [requires_fcv_62, featureFlagConnHealthMetrics]
 */
(function() {
"use strict";

load('jstests/libs/session_auth_metrics_helpers.js');

let rootCreds = {user: 'root', pwd: 'root'};
let conn = MongoRunner.runMongod({auth: ''});

jsTestLog("Setting up users and test data.");
let runTest = sessionAuthMetricsTest(
    conn, {preAuthDelayMillis: 50, postAuthDelayMillis: 100, rootCredentials: rootCreds});

jsTestLog("Connecting to mongod and running the test.");
runTest();

MongoRunner.stopMongod(conn, null, rootCreds);
})();
