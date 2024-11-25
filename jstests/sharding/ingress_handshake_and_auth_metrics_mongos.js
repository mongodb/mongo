/**
 * Tests for the ingress handshake metrics.
 *
 * @tags: [
 *   requires_fcv_70,
 *   # TODO (SERVER-97257): Re-enable this test.
 *   # Test doesn't start enough mongods to have num_mongos routers
 *   embedded_router_incompatible,
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
        helloProcessingDelayMillis: 50,
        helloResponseDelayMillis: 100,
        helloFailPointName: "routerWaitInHello",
    });

    jsTestLog("Connecting to mongos and running the test.");
    runMetricsTest();

    st.stop();
};

// Parameterized on enabling/disabling connection health logging.
runTest(true);
runTest(false);
