// Tests that dropping and re-adding a shard with the same name to a cluster doesn't mess up
// migrations
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {removeShard} from "jstests/sharding/libs/remove_shard_util.js";

// TODO SERVER-50144 Remove this and allow orphan checking.
// This test calls removeShard which can leave docs in config.rangeDeletions in state "pending",
// therefore preventing orphans from being cleaned up.
TestData.skipCheckOrphans = true;

var st = new ShardingTest({shards: 2, mongos: 1});

var mongos = st.s;
var admin = mongos.getDB('admin');
var coll = mongos.getCollection('foo.bar');

// Shard collection with initial chunk on shard0
assert.commandWorked(mongos.adminCommand(
    {enableSharding: coll.getDB().getName(), primaryShard: st.shard0.shardName}));
assert.commandWorked(mongos.adminCommand({shardCollection: coll + '', key: {_id: 1}}));

// Insert one document
assert.commandWorked(coll.insert({hello: 'world'}));

// Migrate the collection to and from shard1 so shard0 loads the shard1 host
assert.commandWorked(mongos.adminCommand(
    {moveChunk: coll + '', find: {_id: 0}, to: st.shard1.shardName, _waitForDelete: true}));
assert.commandWorked(mongos.adminCommand(
    {moveChunk: coll + '', find: {_id: 0}, to: st.shard0.shardName, _waitForDelete: true}));

// Guarantee the sessions collection chunk isn't on shard1.
assert.commandWorked(mongos.adminCommand({
    moveChunk: "config.system.sessions",
    find: {_id: 0},
    to: st.shard0.shardName,
    _waitForDelete: true
}));

// Drop and re-add shard with the same name but a new host.
removeShard(st, st.shard1.shardName);

let shard2 = new ReplSetTest({nodes: 2, nodeOptions: {shardsvr: ""}});
shard2.startSet();
shard2.initiate();

assert.commandWorked(mongos.adminCommand({addShard: shard2.getURL(), name: st.shard1.shardName}));

jsTest.log('Shard was dropped and re-added with same name...');
st.printShardingStatus();

// Try a migration
assert.commandWorked(
    mongos.adminCommand({moveChunk: coll + '', find: {_id: 0}, to: st.shard1.shardName}));

let shard2Conn = shard2.getPrimary();
assert.eq('world', shard2Conn.getCollection(coll + '').findOne().hello);

st.stop();
shard2.stopSet();