/**
 * Tests for the ingress handshake metrics.
 *
 * These tests are tagged "multiversion_incompatible" because they assert the
 * presence of some metrics that are not necessarily present on older servers.
 *
 * @tags: [
 *   multiversion_incompatible,
 *   requires_fcv_70,
 * ]
 */
import {ingressHandshakeMetricsTest} from "jstests/libs/ingress_handshake_metrics_helpers.js";

let runTest = (connectionHealthLoggingOn) => {
    let rootCreds = {user: 'root', pwd: 'root'};
    let conn = MongoRunner.runMongod({auth: ''});

    jsTestLog("Setting up users and test data.");
    let runMetricsTest = ingressHandshakeMetricsTest(conn, {
        connectionHealthLoggingOn: connectionHealthLoggingOn,
        preAuthDelayMillis: 50,
        postAuthDelayMillis: 100,
        rootCredentials: rootCreds,
    });

    jsTestLog("Connecting to mongod and running the test.");
    runMetricsTest();

    MongoRunner.stopMongod(conn, null, rootCreds);
};

// Parameterized on enabling/disabling connection health logging.
runTest(true);
runTest(false);
