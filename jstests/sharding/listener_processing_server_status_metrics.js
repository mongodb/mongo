/**
 *  @tags: [requires_fcv_60, featureFlagConnHealthMetrics, __TEMPORARILY_DISABLED__]
 *
 * Tests that listener processing times for connections are properly reported in server status
 * metrics. With each new connection to the same host, the value of the metric should be
 * monotonically increasing.
 */

(function() {
"use strict";

const numConnections = 10;
const st = new ShardingTest({shards: 1, mongos: 1});
const admin = st.s.getDB("admin");

let previous = 0;
for (var i = 0; i < numConnections; i++) {
    const conn = new Mongo(admin.getMongo().host);
    const t =
        assert.commandWorked(admin.serverStatus()).network.listenerProcessingTime["durationMicros"];
    assert.gte(t, previous);
    previous = t;
}

// While there is an off-chance that the timer doesn't increment for a connection, this is rare and
// the metric should have increased for most if not all connections.
assert.gt(previous, 0);

st.stop();
})();
