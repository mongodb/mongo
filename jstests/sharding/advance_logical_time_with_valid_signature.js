/**
 * Tests that the mongo shell can use a cluster time with a valid signature to advance a server's
 * cluster time.
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";

// Setup 2 mongos processes with mongobridge.
let st = new ShardingTest({shards: 1, mongos: 2, useBridge: true});

// Sever outgoing communications from the second mongos.
st.s0.disconnect(st.s1);
st.forEachConfigServer(function (configSvr) {
    configSvr.disconnect(st.s1);
});

st._rsObjects.forEach(function (rsNodes) {
    rsNodes.nodes.forEach(function (conn) {
        conn.disconnect(st.s1);
    });
});

let connectedDB = st.s0.getDB("test");
let disconnectedDB = st.s1.getDB("test");

// Send an insert to the connected mongos to advance its cluster time.
let res = assert.commandWorked(connectedDB.runCommand({insert: "foo", documents: [{x: 1}]}));

// Get logicalTime metadata from the connected mongos's response and send it in a hello
// command to the disconnected mongos. hello does not require mongos to contact any other
// servers, so the command should succeed.
let lt = res.$clusterTime;
res = assert.commandWorked(
    disconnectedDB.runCommand({hello: 1, $clusterTime: lt}),
    "expected the disconnected mongos to accept cluster time: " + tojson(lt),
);

// Verify cluster time response from the disconnected mongos matches what was passed.
assert.eq(
    lt,
    res.$clusterTime,
    "expected the disconnected mongos to send cluster time: " + tojson(lt) + ", received: " + tojson(res.$clusterTime),
);

st.stop();
