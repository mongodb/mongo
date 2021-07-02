//
// Test the listShards command by adding stand-alone and replica-set shards to a cluster
//
(function() {
'use strict';

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

const shardTest =
    new ShardingTest({name: 'listShardsTest', shards: 1, mongos: 1, other: {useHostname: true}});

const mongos = shardTest.s0;
let res = mongos.adminCommand('listShards');
assert.commandWorked(res, 'listShards command failed');
let shardsArray = res.shards;
assert.eq(shardsArray.length, 1);

// add replica set named 'repl'
const rs1 =
    new ReplSetTest({name: 'repl', nodes: 1, useHostName: true, nodeOptions: {shardsvr: ""}});
rs1.startSet();
rs1.initiate();
res = shardTest.admin.runCommand({addShard: rs1.getURL()});
assert.commandWorked(res, 'addShard command failed');
res = mongos.adminCommand('listShards');
assert.commandWorked(res, 'listShards command failed');
shardsArray = res.shards;
assert.eq(shardsArray.length, 2);
assert(checkShardName('repl', shardsArray),
       'listShards command didn\'t return replica set shard: ' + tojson(shardsArray));

// remove 'repl' shard
assert.soon(function() {
    var res = shardTest.admin.runCommand({removeShard: 'repl'});
    if (!res.ok && res.code === ErrorCodes.ShardNotFound) {
        // If the config server primary steps down right after removing the config.shards doc
        // for the shard but before responding with "state": "completed", the mongos would retry
        // the _configsvrRemoveShard command against the new config server primary, which would
        // not find the removed shard in its ShardRegistry if it has done a ShardRegistry reload
        // after the config.shards doc for the shard was removed. This would cause the command
        // to fail with ShardNotFound.
        return true;
    }
    assert.commandWorked(res, 'removeShard command failed');
    return res.state === 'completed';
}, 'failed to remove the replica set shard');

res = mongos.adminCommand('listShards');
assert.commandWorked(res, 'listShards command failed');
shardsArray = res.shards;
assert.eq(shardsArray.length, 1);
assert(!checkShardName('repl', shardsArray),
       'listShards command returned removed replica set shard: ' + tojson(shardsArray));

rs1.stopSet();
shardTest.stop();
})();
