/**
 * Tests for the ingress handshake metrics.
 *
 * @tags: [
 *   requires_fcv_70,
 *   # Test doesn't start enough mongods to have num_mongos routers
 *   temp_disabled_embedded_router,
 * ]
 */
import {getFailPointName} from "jstests/libs/fail_point_util.js";
import {ingressHandshakeMetricsTest} from "jstests/libs/ingress_handshake_metrics_helpers.js";

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
        helloFailPointName: getFailPointName("routerWaitInHello", conn.getMaxWireVersion()),
    });

    jsTestLog("Connecting to mongos and running the test.");
    runMetricsTest();

    st.stop();
};

// Parameterized on enabling/disabling connection health logging.
runTest(true);
runTest(false);
