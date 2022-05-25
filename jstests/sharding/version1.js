(function() {
'use strict';

load("jstests/sharding/libs/find_chunks_util.js");

var s = new ShardingTest({name: "version1", shards: 1});

assert.commandWorked(s.s0.adminCommand({enablesharding: "alleyinsider"}));
assert.commandWorked(s.s0.adminCommand({shardcollection: "alleyinsider.foo", key: {num: 1}}));

var a = s.shard0.getDB("admin");

assert.commandFailed(a.runCommand({setShardVersion: "alleyinsider.foo", configdb: s._configDB}));

assert.commandFailed(
    a.runCommand({setShardVersion: "alleyinsider.foo", configdb: s._configDB, version: "a"}));

assert.commandFailed(a.runCommand(
    {setShardVersion: "alleyinsider.foo", configdb: s._configDB, authoritative: true}));

assert.commandFailed(
    a.runCommand(
        {setShardVersion: "alleyinsider.foo", configdb: s._configDB, version: new Timestamp(2, 0)}),
    "should have failed b/c no auth");

assert.commandFailed(a.runCommand({
    setShardVersion: "alleyinsider.foo",
    configdb: s._configDB,
    version: {e: epoch, t: timestamp, v: new Timestamp(2, 0)},
    authoritative: true
}),
                     "should have failed because first setShardVersion needs shard info");

assert.commandFailed(a.runCommand({
    setShardVersion: "alleyinsider.foo",
    configdb: s._configDB,
    version: {e: epoch, t: timestamp, v: new Timestamp(2, 0)},
    authoritative: true,
    shard: "s.shard0.shardName",
    shardHost: s.s.host
}),
                     "should have failed because version is config is 1|0");

var epoch = s.getDB('config').collections.findOne({_id: "alleyinsider.foo"}).lastmodEpoch;
var timestamp = s.getDB('config').collections.findOne({_id: "alleyinsider.foo"}).timestamp;
assert.commandWorked(a.runCommand({
    setShardVersion: "alleyinsider.foo",
    configdb: s._configDB,
    version: {e: epoch, t: timestamp, v: new Timestamp(1, 0)},
    authoritative: true,
    shard: s.shard0.shardName,
    shardHost: s.s.host
}),
                     "should have worked");

assert.commandFailed(a.runCommand({
    setShardVersion: "alleyinsider.foo",
    configdb: "a",
    version: {e: epoch, t: timestamp, v: new Timestamp(0, 2)},
}));

assert.commandFailed(a.runCommand({
    setShardVersion: "alleyinsider.foo",
    configdb: s._configDB,
    version: {e: epoch, t: timestamp, v: new Timestamp(0, 2)},
}));

assert.commandFailed(a.runCommand({
    setShardVersion: "alleyinsider.foo",
    configdb: s._configDB,
    version: {e: epoch, t: timestamp, v: new Timestamp(0, 1)},
}));

s.stop();
})();
