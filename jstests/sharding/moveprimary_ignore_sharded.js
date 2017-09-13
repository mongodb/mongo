// Checks that movePrimary doesn't move collections detected as sharded when it begins moving
var st = new ShardingTest({shards: 2, mongos: 2, verbose: 1});

// Stop balancer, otherwise mongosB may load information about the database non-deterministically
st.stopBalancer();

var mongosA = st.s0;
var mongosB = st.s1;

var adminA = mongosA.getDB("admin");
var adminB = mongosB.getDB("admin");

var configA = mongosA.getDB("config");
var configB = mongosB.getDB("config");

// Populate some data
assert.writeOK(mongosA.getCollection("foo.coll0").insert({hello: "world"}));
assert.writeOK(mongosA.getCollection("bar.coll0").insert({hello: "world"}));
assert.writeOK(mongosA.getCollection("foo.coll1").insert({hello: "world"}));
assert.writeOK(mongosA.getCollection("bar.coll1").insert({hello: "world"}));
assert.writeOK(mongosA.getCollection("foo.coll2").insert({hello: "world"}));
assert.writeOK(mongosA.getCollection("bar.coll2").insert({hello: "world"}));

// Enable sharding
printjson(adminA.runCommand({enableSharding: "foo"}));
st.ensurePrimaryShard('foo', 'shard0001');
printjson(adminA.runCommand({enableSharding: "bar"}));
st.ensurePrimaryShard('bar', 'shard0000');

// Setup three collections per-db
// 0 : not sharded
// 1 : sharded
// 2 : sharded but not seen as sharded by mongosB
printjson(adminA.runCommand({shardCollection: "foo.coll1", key: {_id: 1}}));
printjson(adminA.runCommand({shardCollection: "foo.coll2", key: {_id: 1}}));
printjson(adminA.runCommand({shardCollection: "bar.coll1", key: {_id: 1}}));
printjson(adminA.runCommand({shardCollection: "bar.coll2", key: {_id: 1}}));

// All collections are now on primary shard
var fooPrimaryShardId = configA.databases.findOne({_id: "foo"}).primary;
var barPrimaryShardId = configA.databases.findOne({_id: "bar"}).primary;

var fooPrimaryShard = (fooPrimaryShardId == st.shard0.shardName) ? st.shard0 : st.shard1;
var fooOtherShard = (fooPrimaryShard.shardName == st.shard0.shardName) ? st.shard1 : st.shard0;
var barPrimaryShard = (barPrimaryShardId == st.shard0.shardName) ? st.shard0 : st.shard1;
var barOtherShard = (barPrimaryShard.shardName == st.shard0.shardName) ? st.shard1 : st.shard0;

st.printShardingStatus();

jsTest.log("Running movePrimary for foo through mongosA ...");

// MongosA should already know about all the collection states
printjson(adminA.runCommand({movePrimary: "foo", to: fooOtherShard.shardName}));

if (st.configRS) {
    // If we are in CSRS mode need to make sure that mongosB will actually get the most recent
    // config data.
    st.configRS.awaitLastOpCommitted();
}

// All collections still correctly sharded / unsharded
assert.neq(null, mongosA.getCollection("foo.coll0").findOne());
assert.neq(null, mongosA.getCollection("foo.coll1").findOne());
assert.neq(null, mongosA.getCollection("foo.coll2").findOne());

assert.neq(null, mongosB.getCollection("foo.coll0").findOne());
assert.neq(null, mongosB.getCollection("foo.coll1").findOne());
assert.neq(null, mongosB.getCollection("foo.coll2").findOne());

function realCollectionCount(mydb) {
    var num = 0;
    mydb.getCollectionNames().forEach(function(z) {
        if (z.startsWith("coll"))
            num++;
    });
    return num;
}

// All collections sane
assert.eq(2, realCollectionCount(fooPrimaryShard.getDB("foo")));
assert.eq(1, realCollectionCount(fooOtherShard.getDB("foo")));

jsTest.log("Running movePrimary for bar through mongosB ...");
printjson(adminB.runCommand({movePrimary: "bar", to: barOtherShard.shardName}));

// We need to flush the cluster config on mongosA, so it can discover that database 'bar' got
// moved. Otherwise since the collections are not sharded, we have no way of discovering this.
// See SERVER-8059.
if (st.configRS) {
    // If we are in CSRS mode need to make sure that after we flushRouterConfig we'll actually get
    // the most recent config data.
    st.configRS.awaitLastOpCommitted();
}
assert.commandWorked(adminA.runCommand({flushRouterConfig: 1}));

// All collections still correctly sharded / unsharded
assert.neq(null, mongosA.getCollection("bar.coll0").findOne());
assert.neq(null, mongosA.getCollection("bar.coll1").findOne());
assert.neq(null, mongosA.getCollection("bar.coll2").findOne());

assert.neq(null, mongosB.getCollection("bar.coll0").findOne());
assert.neq(null, mongosB.getCollection("bar.coll1").findOne());
assert.neq(null, mongosB.getCollection("bar.coll2").findOne());

// All collections sane
assert.eq(2, realCollectionCount(barPrimaryShard.getDB("bar")));
assert.eq(1, realCollectionCount(barOtherShard.getDB("bar")));

st.stop();
