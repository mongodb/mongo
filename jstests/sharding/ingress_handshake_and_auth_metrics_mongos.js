/**
 * Tests for the ingress handshake metrics.
 *
 * @tags: [requires_fcv_63]
 */
(function() {
"use strict";

load('jstests/libs/ingress_handshake_metrics_helpers.js');

let st = new ShardingTest({shards: 0, other: {auth: ''}});
let conn = st.s;

jsTestLog("Setting up users and test data.");
let runTest = ingressHandshakeMetricsTest(conn, {
    preAuthDelayMillis: 50,
    postAuthDelayMillis: 100,
    helloProcessingDelayMillis: 50,
    helloResponseDelayMillis: 100
});

jsTestLog("Connecting to mongos and running the test.");
runTest();

st.stop();
})();
