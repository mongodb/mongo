/**
 * Tests for the connection establishment metrics.
 *
 * @tags: [requires_fcv_62, featureFlagConnHealthMetrics]
 */
(function() {
"use strict";

load('jstests/libs/session_auth_metrics_helpers.js');

let st = new ShardingTest({shards: 0, other: {auth: ''}});
let conn = st.s;

jsTestLog("Setting up users and test data.");
let runTest = sessionAuthMetricsTest(conn, {preAuthDelayMillis: 50, postAuthDelayMillis: 100});

jsTestLog("Connecting to mongos and running the test.");
runTest();

st.stop();
})();
