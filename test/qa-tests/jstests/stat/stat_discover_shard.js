(function() {

load("jstests/libs/mongostat.js");

baseName = "tool_discover_shard";

st = new ShardingTest({name:"shard1", shards:2});

stdb = st.getDB("test");

shardPorts = [ st._mongos[0].port, st._connections[0].port, st._connections[1].port ];

clearRawMongoProgramOutput();

pid = startMongoProgramNoConnect("mongostat", "--host", st._mongos[0].host, "--discover");

sleep(5000);

assert(statOutputPortCheck(shardPorts), "--discover against a mongos sees all shards");

st.stop();

// FIXME currently, on windows, stopMongoProgramByPid doesn't terminiate a process in a way that it can control it's exit code
// so the return of stopMongoProgramByPid will probably be 1 in either case.
assert.eq(_isWindows() ? 1 : exitCodeStopped, stopMongoProgramByPid(pid), "mongostat --discover against a sharded cluster shouldn't error when the cluster goes down");

}());
