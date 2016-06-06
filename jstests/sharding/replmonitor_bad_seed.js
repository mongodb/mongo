/**
 * This tests tries to check that a ReplicaSetMonitor initialized with a
 * replica set seed that has none of the nodes up will be able to recover
 * once the replica set come back up.
 *
 * ReplSetMonitor is tested indirectly through mongos. This is because
 * attempting to create a connection through the Mongo constructor won't
 * work because the shell will throw an exception before the mongo shell
 * binds the variable properly to the js environment (in simple terms,
 * the connection object is never returned when it cannot connect to it).
 * Another reason for using mongos in this test is so we can use
 * connPoolStats to synchronize the test and make sure that the monitor
 * was able to refresh before proceeding to check.
 */
(function() {
    'use strict';

    var st = new ShardingTest({shards: 1, rs: {oplogSize: 10}});
    var replTest = st.rs0;

    assert.commandWorked(st.s0.adminCommand({enableSharding: 'test'}));
    assert.commandWorked(st.s0.adminCommand({shardCollection: 'test.user', key: {x: 1}}));

    // The cluster now has the shard information. Then kill the replica set so when mongos restarts
    // and tries to create a ReplSetMonitor for that shard, it will not be able to connect to any of
    // the seed servers.
    replTest.stopSet();

    st.restartMongos(0);

    replTest.startSet({oplogSize: 10});
    replTest.initiate();
    replTest.awaitSecondaryNodes();

    // Verify that the replSetMonitor can reach the restarted set
    ReplSetTest.awaitRSClientHosts(st.s0, replTest.nodes, {ok: true});

    assert.writeOK(st.s0.getDB('test').user.insert({x: 1}));

    st.stop();
})();
