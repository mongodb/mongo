/**
 *  @tags: [requires_fcv_60, featureFlagConnHealthMetrics, __TEMPORARILY_DISABLED__]
 *
 * Tests that listener processing times for connections are properly reported in server status
 * metrics. With each new connection to the same host, the value of the metric should always
 * be strictly increasing to reflect its rolling sum nature.
 */

(function() {
"use strict";

const numConnections = 10;
const st = new ShardingTest({shards: 1, mongos: 1});
const admin = st.s.getDB("admin");

assert(admin.adminCommand({getParameter: 1, featureFlagConnHealthMetrics: 1})
           .featureFlagConnHealthMetrics.value,
       'featureFlagConnHealthMetrics should be enabled for this test');

const uri = "mongodb://" + admin.getMongo().host;
const testDB = "listenerProcessingTest";

let previous = 0;
for (var i = 0; i < numConnections; i++) {
    const conn = new Mongo(uri);
    const t =
        assert.commandWorked(admin.serverStatus()).network.listenerProcessingTime["durationMicros"];
    assert.gt(t, previous);
    previous = t;
}

st.stop();
})();
