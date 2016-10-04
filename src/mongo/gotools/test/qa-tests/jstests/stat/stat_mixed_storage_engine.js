// @tags: [requires_mmap_available]
(function() {
  if (TestData && TestData.storageEngine === 'wiredTiger') {
    return;
  }
  load("jstests/libs/mongostat.js");
  load('jstests/libs/extended_assert.js');
  var assert = extendedAssert;

  var mmap_options = {storageEngine: "mmapv1"};
  var wt_options = {storageEngine: "wiredTiger"};
  var replTest = new ReplSetTest({
    nodes: {
      node0: mmap_options,
      node1: mmap_options,
      node2: wt_options,
    },
  });

  replTest.startSet();
  replTest.initiate();
  replTest.awaitReplication();

  clearRawMongoProgramOutput();
  assert(discoverTest(replTest.ports, replTest.nodes[0].host), "mongostat against a heterogenous storage engine replica set sees all hosts");

  clearRawMongoProgramOutput();
  runMongoProgram("mongostat", "--host", replTest.nodes[0].host, "--rowcount", 7, "--discover");
  assert.strContains.soon("used flushes mapped", rawMongoProgramOutput, "against replset has fields for both engines");

  replTest.stopSet();

  st = new ShardingTest({shards: [wt_options, mmap_options], options: {nopreallocj: true}});
  stdb = st.getDB("test");
  shardPorts = [st._mongos[0].port, st._connections[0].port, st._connections[1].port];

  clearRawMongoProgramOutput();
  assert(discoverTest(shardPorts, st._mongos[0].host, "mongostat reports on a heterogenous storage engine sharded cluster"));

  clearRawMongoProgramOutput();
  runMongoProgram("mongostat", "--host", st._mongos[0].host, "--rowcount", 7, "--discover");
  assert.strContains.soon("used flushes mapped", rawMongoProgramOutput, "against sharded cluster has fields for both engines");

  st.stop();
}());
