//
// Test the listShards command by adding stand-alone and replica-set shards to a cluster
//
(function() {
    'use strict';

    function checkShardName(shardName, shardsArray) {
        var found = false;
        shardsArray.forEach(function(shardObj) {
            if (shardObj._id === shardName) {
                found = true;
                return;
            }
        });
        return found;
    }

    var shardTest = new ShardingTest(
        {name: 'listShardsTest', shards: 1, mongos: 1, other: {useHostname: true}});

    var mongos = shardTest.s0;
    var res = mongos.adminCommand('listShards');
    assert.commandWorked(res, 'listShards command failed');
    var shardsArray = res.shards;
    assert.eq(shardsArray.length, 1);

    // add standalone mongod
    var standaloneShard = MongoRunner.runMongod({useHostName: true});
    res = shardTest.admin.runCommand({addShard: standaloneShard.host, name: 'standalone'});
    assert.commandWorked(res, 'addShard command failed');
    res = mongos.adminCommand('listShards');
    assert.commandWorked(res, 'listShards command failed');
    shardsArray = res.shards;
    assert.eq(shardsArray.length, 2);
    assert(checkShardName('standalone', shardsArray),
           'listShards command didn\'t return standalone shard: ' + tojson(shardsArray));

    // add replica set named 'repl'
    var rs1 = new ReplSetTest({name: 'repl', nodes: 1, useHostName: true});
    rs1.startSet();
    rs1.initiate();
    res = shardTest.admin.runCommand({addShard: rs1.getURL()});
    assert.commandWorked(res, 'addShard command failed');
    res = mongos.adminCommand('listShards');
    assert.commandWorked(res, 'listShards command failed');
    shardsArray = res.shards;
    assert.eq(shardsArray.length, 3);
    assert(checkShardName('repl', shardsArray),
           'listShards command didn\'t return replica set shard: ' + tojson(shardsArray));

    // remove 'repl' shard
    assert.soon(function() {
        var res = shardTest.admin.runCommand({removeShard: 'repl'});
        assert.commandWorked(res, 'removeShard command failed');
        return res.state === 'completed';
    }, 'failed to remove the replica set shard');

    res = mongos.adminCommand('listShards');
    assert.commandWorked(res, 'listShards command failed');
    shardsArray = res.shards;
    assert.eq(shardsArray.length, 2);
    assert(!checkShardName('repl', shardsArray),
           'listShards command returned removed replica set shard: ' + tojson(shardsArray));

    rs1.stopSet();
    shardTest.stop();

})();
