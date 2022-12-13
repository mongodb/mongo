/**
 * Tests that we are able to log the metrics corresponding to the time it takes from egress
 * connection acquisition to writing to the wire.
 *
 * @tags: [requires_fcv_63]
 */
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/log.js");
load("jstests/libs/parallel_shell_helpers.js");

function getConnAcquiredToWireMicros(conn) {
    return conn.adminCommand({serverStatus: 1})
        .metrics.network.totalTimeForEgressConnectionAcquiredToWireMicros;
}

const setParamOptions = {
    "failpoint.alwaysLogConnAcquisitionToWireTime": tojson({mode: "alwaysOn"}),
    logComponentVerbosity: tojson({network: {verbosity: 2}})
};

const st = new ShardingTest({
    shards: 1,
    rs: {nodes: 1, setParameter: setParamOptions},
    mongos: 1,
    mongosOptions: {setParameter: setParamOptions}
});
let initialConnAcquiredToWireTime = getConnAcquiredToWireMicros(st.s);
jsTestLog(`Initial metric value for mongos totalTimeForEgressConnectionAcquiredToWireMicros: ${
    tojson(initialConnAcquiredToWireTime)}`);
assert.commandWorked(st.s.adminCommand({clearLog: 'global'}));

// The RSM will periodically acquire egress connections to ping the shard and config server nodes,
// but we do an insert to speed up the wait and to be more explicit.
assert.commandWorked(st.s.getDB(jsTestName())["test"].insert({x: 1}));
checkLog.containsJson(st.s, 6496702);
let afterConnAcquiredToWireTime = getConnAcquiredToWireMicros(st.s);
jsTestLog(`End metric value for mongos totalTimeForEgressConnectionAcquiredToWireMicros: ${
    tojson(afterConnAcquiredToWireTime)}`);
assert.gt(afterConnAcquiredToWireTime,
          initialConnAcquiredToWireTime,
          st.s.adminCommand({serverStatus: 1}));

// Test with mirrored reads to execute the 'fireAndForget' path and verify logs are still correctly
// printed.
const shardPrimary = st.rs0.getPrimary();
assert.commandWorked(shardPrimary.adminCommand({clearLog: 'global'}));
initialConnAcquiredToWireTime = getConnAcquiredToWireMicros(shardPrimary);
jsTestLog(`Initial metric value for mongod totalTimeForEgressConnectionAcquiredToWireMicros: ${
    tojson(initialConnAcquiredToWireTime)}`);
assert.commandWorked(
    shardPrimary.adminCommand({setParameter: 1, mirrorReads: {samplingRate: 1.0}}));
shardPrimary.getDB(jsTestName()).runCommand({find: "test", filter: {}});
checkLog.containsJson(shardPrimary, 6496702);
afterConnAcquiredToWireTime = getConnAcquiredToWireMicros(shardPrimary);
jsTestLog(`End metric value for mongod totalTimeForEgressConnectionAcquiredToWireMicros: ${
    tojson(afterConnAcquiredToWireTime)}`);
assert.gt(afterConnAcquiredToWireTime,
          initialConnAcquiredToWireTime,
          shardPrimary.adminCommand({serverStatus: 1}));
st.stop();
})();
