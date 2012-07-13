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

var rsOpt = { oplogSize: 10 };
var st = new ShardingTest({ shards: 1, rs: rsOpt });
var mongos = st.s;
var replTest = st.rs0;

var adminDB = mongos.getDB('admin');
//adminDB.runCommand({ addShard: replTest.getURL() });

adminDB.runCommand({ enableSharding: 'test' });
adminDB.runCommand({ shardCollection: 'test.user', key: { x: 1 }});

/* The cluster now has the shard information. Then kill the replica set so
 * when mongos restarts and tries to create a ReplSetMonitor for that shard,
 * it will not be able to connect to any of the seed servers.
 */
replTest.stopSet();
st.restartMongos(0);
mongos = st.s; // refresh mongos with the new one

var coll = mongos.getDB('test').user;

var verifyInsert = function() {
    var beforeCount = coll.find().count();
    coll.insert({ x: 1 });
    coll.getDB().getLastError();
    var afterCount = coll.find().count();

    assert.eq(beforeCount + 1, afterCount);
};

jsTest.log('Insert to a downed replSet');
assert.throws(verifyInsert);

replTest.startSet({ oplogSize: 10 });
replTest.initiate();
replTest.awaitSecondaryNodes();

jsTest.log('Insert to an online replSet');

// Verify that the replSetMonitor can reach the restarted set.
ReplSetTest.awaitRSClientHosts(mongos, replTest.nodes, { ok: true });
verifyInsert();

st.stop();

