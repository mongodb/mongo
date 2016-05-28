//
// Tests that a balancer round loads newly sharded collection data
//

var options = {mongosOptions: {verbose: 1}};

var st = new ShardingTest({shards: 2, mongos: 2, other: options});

// Stop balancer initially
st.stopBalancer();

var mongos = st.s0;
var staleMongos = st.s1;
var coll = mongos.getCollection("foo.bar");

// Shard collection through first mongos
assert(mongos.adminCommand({enableSharding: coll.getDB() + ""}).ok);
st.ensurePrimaryShard(coll.getDB().getName(), 'shard0001');
assert(mongos.adminCommand({shardCollection: coll + "", key: {_id: 1}}).ok);

// Create a bunch of chunks
var numSplits = 20;
for (var i = 0; i < numSplits; i++) {
    assert(mongos.adminCommand({split: coll + "", middle: {_id: i}}).ok);
}

// Stop the first mongos who setup the cluster.
st.stopMongos(0);

// Start balancer, which lets the stale mongos balance
assert.writeOK(
    staleMongos.getDB("config").settings.update({_id: "balancer"}, {$set: {stopped: false}}, true));

// Make sure we eventually start moving chunks
assert.soon(function() {
    return staleMongos.getCollection("config.changelog").count({what: /moveChunk/}) > 0;
}, "no balance happened", 5 * 60 * 1000);

jsTest.log("DONE!");

st.stop();
