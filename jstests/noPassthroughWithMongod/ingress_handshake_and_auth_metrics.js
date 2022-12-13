/**
 * Tests for the ingress handshake metrics.
 *
 * @tags: [requires_fcv_63]
 */
(function() {
"use strict";

load('jstests/libs/ingress_handshake_metrics_helpers.js');

let rootCreds = {user: 'root', pwd: 'root'};
let conn = MongoRunner.runMongod({auth: ''});

jsTestLog("Setting up users and test data.");
let runTest = ingressHandshakeMetricsTest(conn, {
    preAuthDelayMillis: 50,
    postAuthDelayMillis: 100,
    helloProcessingDelayMillis: 50,
    helloResponseDelayMillis: 100,
    rootCredentials: rootCreds
});

jsTestLog("Connecting to mongod and running the test.");
runTest();

MongoRunner.stopMongod(conn, null, rootCreds);
})();
