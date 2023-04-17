// Test that having replica set names the same as the names of other shards works fine
(function() {
'use strict';

var st = new ShardingTest({shards: TestData.configShard ? 1 : 0, mongos: 1});

var rsA = new ReplSetTest({nodes: 2, name: "rsA", nodeOptions: {shardsvr: ""}});
var rsB = new ReplSetTest({nodes: 2, name: "rsB", nodeOptions: {shardsvr: ""}});

rsA.startSet();
rsB.startSet();
rsA.initiate();
rsB.initiate();
rsA.getPrimary();
rsB.getPrimary();

var mongos = st.s;
var config = mongos.getDB("config");
var admin = mongos.getDB("admin");

assert.commandWorked(mongos.adminCommand({addShard: rsA.getURL(), name: rsB.name}));
printjson(config.shards.find().toArray());

assert.commandWorked(mongos.adminCommand({addShard: rsB.getURL(), name: rsA.name}));
printjson(config.shards.find().toArray());

assert.eq(TestData.configShard ? 3 : 2, config.shards.count(), "Error adding a shard");
assert.eq(rsB.getURL(), config.shards.findOne({_id: rsA.name})["host"], "Wrong host for shard rsA");
assert.eq(rsA.getURL(), config.shards.findOne({_id: rsB.name})["host"], "Wrong host for shard rsB");

// Remove shard
assert.commandWorked(mongos.adminCommand({removeshard: rsA.name}),
                     "failed to start draining shard");
var res =
    assert.commandWorked(mongos.adminCommand({removeshard: rsA.name}), "failed to remove shard");

assert.eq(TestData.configShard ? 2 : 1,
          config.shards.count(),
          "Shard was not removed: " + res + "; Shards: " + tojson(config.shards.find().toArray()));
assert.eq(
    rsA.getURL(), config.shards.findOne({_id: rsB.name})["host"], "Wrong host for shard rsB 2");

// Re-add shard; the command may need to be retried as the request may be not be immediately
// fulfillable after the completion of a removeShard (the RSM associated to the removed shard may
// still raise some spurious failure as it gets dropped).
assert.soon(() => {
    let res = mongos.adminCommand({addShard: rsB.getURL(), name: rsA.name});
    return res.ok;
});

printjson(config.shards.find().toArray());

assert.eq(TestData.configShard ? 3 : 2, config.shards.count(), "Error re-adding a shard");
assert.eq(
    rsB.getURL(), config.shards.findOne({_id: rsA.name})["host"], "Wrong host for shard rsA 3");
assert.eq(
    rsA.getURL(), config.shards.findOne({_id: rsB.name})["host"], "Wrong host for shard rsB 3");

rsA.stopSet();
rsB.stopSet();
st.stop();
})();
