//
// Test the listShards command by adding stand-alone and replica-set shards to a cluster
//
(function() {
'use strict';
load('jstests/sharding/libs/remove_shard_util.js');

// TODO SERVER-50144 Remove this and allow orphan checking.
// This test calls removeShard which can leave docs in config.rangeDeletions in state "pending",
// therefore preventing orphans from being cleaned up.
TestData.skipCheckOrphans = true;

const checkShardName = function(shardName, shardsArray) {
    var found = false;
    shardsArray.forEach((shardObj) => {
        if (shardObj._id === shardName) {
            found = true;
            return;
        }
    });
    return found;
};

const st =
    new ShardingTest({name: 'listShardsTest', shards: 1, mongos: 1, other: {useHostname: true}});

const mongos = st.s0;
let res = mongos.adminCommand('listShards');
assert.commandWorked(res, 'listShards command failed');
let shardsArray = res.shards;
assert.eq(shardsArray.length, 1);

// add replica set named 'repl'
const rs1 =
    new ReplSetTest({name: 'repl', nodes: 1, useHostName: true, nodeOptions: {shardsvr: ""}});
rs1.startSet();
rs1.initiate();
res = st.admin.runCommand({addShard: rs1.getURL()});
assert.commandWorked(res, 'addShard command failed');
res = mongos.adminCommand('listShards');
assert.commandWorked(res, 'listShards command failed');
shardsArray = res.shards;
assert.eq(shardsArray.length, 2);
assert(checkShardName('repl', shardsArray),
       'listShards command didn\'t return replica set shard: ' + tojson(shardsArray));

// remove 'repl' shard
removeShard(st, 'repl');

res = mongos.adminCommand('listShards');
assert.commandWorked(res, 'listShards command failed');
shardsArray = res.shards;
assert.eq(shardsArray.length, 1);
assert(!checkShardName('repl', shardsArray),
       'listShards command returned removed replica set shard: ' + tojson(shardsArray));

rs1.stopSet();
st.stop();
})();
