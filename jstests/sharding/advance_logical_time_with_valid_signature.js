/**
 * Tests that the mongo shell can use a cluster time with a valid signature to advance a server's
 * cluster time.
 */
(function() {
    "use strict";

    // Setup 2 mongos processes with mongobridge.
    let st = new ShardingTest({shards: 1, mongos: 2, useBridge: true, mongosWaitsForKeys: true});

    // Sever outgoing communications from the second mongos.
    st.s0.disconnect(st.s1);
    st._configServers.forEach(function(configSvr) {
        configSvr.disconnect(st.s1);
    });
    st._connections.forEach(function(conn) {
        conn.disconnect(st.s1);
    });

    let connectedDB = st.s0.getDB("test");
    let disconnectedDB = st.s1.getDB("test");

    // Send an insert to the connected mongos to advance its cluster time.
    let res = assert.commandWorked(connectedDB.runCommand({insert: "foo", documents: [{x: 1}]}));

    // Get logicalTime metadata from the connected mongos's response and send it in an isMaster
    // command to the disconnected mongos. isMaster does not require mongos to contact any other
    // servers, so the command should succeed.
    let lt = res.$clusterTime;
    res = assert.commandWorked(
        disconnectedDB.runCommand({isMaster: 1, $clusterTime: lt}),
        "expected the disconnected mongos to accept cluster time: " + tojson(lt));

    // Verify cluster time response from the disconnected mongos matches what was passed.
    assert.eq(lt,
              res.$clusterTime,
              "expected the disconnected mongos to send cluster time: " + tojson(lt) +
                  ", received: " + tojson(res.$clusterTime));

    st.stop();
})();
