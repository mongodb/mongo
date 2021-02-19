(function() {
  load("jstests/libs/mongostat.js");

  var st = new ShardingTest({name: "shard1", shards: 2});
  shardPorts = [st._mongos[0].port, st._connections[0].port, st._connections[1].port];

  clearRawMongoProgramOutput();
  pid = startMongoProgramNoConnect("mongostat", "--host", st._mongos[0].host, "--discover");
  assert.soon(hasOnlyPorts(shardPorts), "--discover against a mongos sees all shards");

  st.stop();
  assert.soon(hasOnlyPorts([]), "stops showing data when hosts come down");
  assert.eq(exitCodeStopped, stopMongoProgramByPid(pid), "mongostat --discover against a sharded cluster shouldn't error when the cluster goes down");
}());
