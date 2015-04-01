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

    var shardTest = new ShardingTest('listShardsTest', 1, 0, 1, { useHostname: true });

    var mongos = shardTest.s0;
    var res = mongos.adminCommand('listShards');
    assert.commandWorked(res, 'listShards command failed');
    var shardsArray = res.shards;
    assert.eq(shardsArray.length, 1);

    // add standalone mongod
    var mongod1Port = 29000;
    startMongodTest(mongod1Port);
    res = shardTest.admin.runCommand({ addShard: getHostName() + ':' + mongod1Port,
                                       name: 'standalone' });
    assert.commandWorked(res, 'addShard command failed');
    res = mongos.adminCommand('listShards');
    assert.commandWorked(res, 'listShards command failed');
    shardsArray = res.shards;
    assert.eq(shardsArray.length, 2);
    assert(checkShardName('standalone', shardsArray),
           'listShards command didn\'t return standalone shard: ' + tojson(shardsArray));

    // add replica set named 'repl'
    var rs1Port = 29001;
    var rs1 = new ReplSetTest({ name: 'repl', nodes: 1, startPort: rs1Port });
    rs1.startSet();
    rs1.initiate();
    res = shardTest.admin.runCommand({ addShard: 'repl/' + getHostName() + ':' + rs1Port });
    assert.commandWorked(res, 'addShard command failed');
    res = mongos.adminCommand('listShards');
    assert.commandWorked(res, 'listShards command failed');
    shardsArray = res.shards;
    assert.eq(shardsArray.length, 3);
    assert(checkShardName('repl', shardsArray),
           'listShards command didn\'t return replica set shard: ' + tojson(shardsArray));

    // remove 'repl' shard
    assert.soon(function() {
        var res = shardTest.admin.runCommand({ removeShard: 'repl' });
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
