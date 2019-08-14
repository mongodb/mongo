(function() {
'use strict';

var s = new ShardingTest({shards: 2});

assert.eq(2, s.config.shards.count(), "server count wrong");

var test1 = s.getDB("test1").foo;
assert.commandWorked(test1.insert({a: 1}));
assert.commandWorked(test1.insert({a: 2}));
assert.commandWorked(test1.insert({a: 3}));
assert.eq(3, test1.count());

assert.commandFailed(s.s0.adminCommand({addshard: "sdd$%", maxTimeMS: 60000}), "Bad hostname");

var portWithoutHostRunning = allocatePort();
assert.commandFailed(
    s.s0.adminCommand({addshard: "127.0.0.1:" + portWithoutHostRunning, maxTimeMS: 60000}),
    "Host which is not up");
assert.commandFailed(
    s.s0.adminCommand({addshard: "10.0.0.1:" + portWithoutHostRunning, maxTimeMS: 60000}),
    "Allowed shard in IP when config is localhost");

s.stop();
})();
