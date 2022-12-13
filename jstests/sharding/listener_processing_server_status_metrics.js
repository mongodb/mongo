/**
 *  @tags: [requires_fcv_63]
 *
 * Tests that listener processing times for connections are properly reported in server status
 * metrics. With each new connection to the same host, the value of the metric should be
 * monotonically increasing.
 */

(function() {
"use strict";

load("jstests/libs/feature_flag_util.js");

const numConnections = 10;
const st = new ShardingTest({shards: 1, mongos: 1});
const admin = st.s.getDB("admin");

if (!FeatureFlagUtil.isEnabled(st.s.getDB("test"), "ConnHealthMetrics")) {
    jsTestLog('Skipping test because the connection health metrics feature flag is disabled.');
    st.stop();
    return;
}

let previous = 0;
for (var i = 0; i < numConnections; i++) {
    const conn = new Mongo(admin.getMongo().host);
    const t =
        assert.commandWorked(admin.serverStatus()).network.listenerProcessingTime["durationMicros"];
    assert.gte(t, previous);
    previous = t;
}

// While there is an off-chance that the timer doesn't increment for a connection, this is rare
// and the metric should have increased for most if not all connections.
assert.gt(previous, 0);

st.stop();
})();
