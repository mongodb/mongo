(function() {

load("jstests/libs/mongostat.js");

baseName = "tool_discover_shard";

st = new ShardingTest("shard1", 2);

stdb = st.getDB("test");

shardPorts = [ st._mongos[0].port, st._shardServers[0].port, st._shardServers[1].port ];

clearRawMongoProgramOutput();

pid = startMongoProgramNoConnect("mongostat", "--host", st._mongos[0].host, "--discover");

sleep(5000);

assert(statOutputPortCheck(shardPorts), "--discover against a mongos sees all shards");

st.stop();

assert.eq(exitCodeStopped, stopMongoProgramByPid(pid), "mongostat --discover against a sharded cluster shouldn't error when the cluster goes down");

}());
