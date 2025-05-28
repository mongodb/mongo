/**
 * Tests for the ingress handshake metrics.
 *
 * These tests are tagged "multiversion_incompatible" because they assert the
 * presence of some metrics that are not necessarily present on older servers.
 *
 * @tags: [
 *   multiversion_incompatible,
 * ]
 */
import {ingressHandshakeMetricsTest} from "jstests/libs/ingress_handshake_metrics_helpers.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

let runTest = (connectionHealthLoggingOn) => {
    let st = new ShardingTest({shards: TestData.configShard ? 1 : 0, other: {auth: ''}});
    let conn = st.s;

    jsTestLog("Setting up users and test data.");
    let runMetricsTest = ingressHandshakeMetricsTest(conn, {
        connectionHealthLoggingOn: connectionHealthLoggingOn,
        preAuthDelayMillis: 50,
        postAuthDelayMillis: 100,
    });

    jsTestLog("Connecting to mongos and running the test.");
    runMetricsTest();

    st.stop();
};

// Parameterized on enabling/disabling connection health logging.
runTest(true);
runTest(false);
