(function() {

if (TestData && TestData.storageEngine === 'wiredTiger')
    return;

load("jstests/libs/mongostat.js");

var mmap_options = {storageEngine: "mmapv1"};

var wt_options = {storageEngine: "wiredTiger"};

var replTest = new ReplSetTest({nodes : {node0 : mmap_options, node1 : mmap_options, node2 : wt_options}});

var conns = replTest.startSet();

replTest.initiate();

replTest.awaitReplication();

clearRawMongoProgramOutput();

assert(discoverTest(replTest.ports, replTest.nodes[0].host), "mongostat against a heterogenous storage engine replica set sees all hosts");

clearRawMongoProgramOutput();

runMongoProgram( "mongostat", "--host", replTest.nodes[0].host, "--rowcount", 1, "--discover" );

assert(rawMongoProgramOutput().match(/used flushes mapped/), "mongostat against mixed storage engine replset has fields corresponding to both engines");

replTest.stopSet();



st = new ShardingTest({shards:[wt_options,mmap_options],options:{nopreallocj:true}})

stdb = st.getDB("test");

shardPorts = [ st._mongos[0].port, st._connections[0].port, st._connections[1].port ];

clearRawMongoProgramOutput();

assert(discoverTest(shardPorts, st._mongos[0].host, "mongostat reports on a heterogenous storage engine sharded cluster"));

clearRawMongoProgramOutput();

// output at least 10 sets of records because the first header may not represent all nodes that may have not been discovered yet
runMongoProgram("mongostat", "--host", st._mongos[0].host, "--rowcount", 11, "--discover");

assert(rawMongoProgramOutput().match(/used flushes mapped/), "mongostat against a heterogenous storage engine sharded cluster sees all hosts");

st.stop();

}());
