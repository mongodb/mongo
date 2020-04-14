// Tests that save style updates correctly change immutable fields
(function() {
'use strict';

var st = new ShardingTest({shards: 2, mongos: 1});

var mongos = st.s;
var config = mongos.getDB("config");
var coll = mongos.getCollection(jsTestName() + ".coll1");

assert.commandWorked(config.adminCommand({enableSharding: coll.getDB() + ""}));
st.ensurePrimaryShard(coll.getDB().getName(), st.shard0.shardName);
assert.commandWorked(config.adminCommand({shardCollection: "" + coll, key: {a: 1}}));

const shard0Coll = st.shard0.getCollection(coll.getFullName());

// Full shard key in save
assert.commandWorked(shard0Coll.save({_id: 1, a: 1}));

// Full shard key on replacement (basically the same as above)
shard0Coll.remove({});
assert.commandWorked(shard0Coll.update({_id: 1}, {a: 1}, true));

// Full shard key after $set
shard0Coll.remove({});
assert.commandWorked(shard0Coll.update({_id: 1}, {$set: {a: 1}}, true));

// Update existing doc (replacement), same shard key value
assert.commandWorked(shard0Coll.update({_id: 1}, {a: 1}));

// Update existing doc ($set), same shard key value
assert.commandWorked(shard0Coll.update({_id: 1}, {$set: {a: 1}}));

st.stop();
})();
