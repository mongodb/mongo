// Tests that mongos and shard mongods can both be started up successfully when there is no config
// server, and that they will wait until there is a config server online before handling any
// sharding operations.
(function() {
"use strict";

var st = new ShardingTest({shards: 2})

jsTestLog("Setting up initial data");

for (var i = 0; i < 100; i++) {
    assert.writeOK(st.s.getDB('test').foo.insert({_id:i}));
}

st.ensurePrimaryShard('test', 'shard0000');

st.adminCommand({enableSharding: 'test'});
st.adminCommand({shardCollection: 'test.foo', key: {_id: 1}});
st.adminCommand({split: 'test.foo', find: {_id: 50}});
st.adminCommand({moveChunk: 'test.foo', find: {_id: 75}, to: 'shard0001'});

// Make sure the pre-existing mongos already has the routing information loaded into memory
assert.eq(100, st.s.getDB('test').foo.find().itcount());

jsTestLog("Shutting down all config servers");
for (var i = 0; i < st._configServers.length; i++) {
    st.stopConfigServer(i);
}

jsTestLog("Starting a new mongos when there are no config servers up");
var newMongosInfo = MongoRunner.runMongos({configdb: st._configDB, waitForConnect: false});
// The new mongos won't accept any new connections, but it should stay up and continue trying
// to contact the config servers to finish startup.
assert.throws(function() { new Mongo(newMongosInfo.host); });


jsTestLog("Restarting a shard while there are no config servers up");
MongoRunner.stopMongod(st.shard1);
st.shard1.restart = true;
MongoRunner.runMongod(st.shard1);

jsTestLog("Queries should fail because the shard can't initialize sharding state");
var error = assert.throws(function() {st.s.getDB('test').foo.find().itcount();});
assert.eq(ErrorCodes.ExceededTimeLimit, error.code);

jsTestLog("Restarting the config servers");
for (var i = 0; i < st._configServers.length; i++) {
    st.restartConfigServer(i);
}

jsTestLog("Queries against the original mongos should work again");
assert.eq(100, st.s.getDB('test').foo.find().itcount());

jsTestLog("Should now be possible to connect to the mongos that was started while the config "
          + "servers were down");
var mongos2 = null;
assert.soon(function() {
                try {
                    mongos2 = new Mongo(newMongosInfo.host);
                    return true;
                } catch (e) {
                    printjson(e);
                    return false;
                }
            });
assert.eq(100, mongos2.getDB('test').foo.find().itcount());

st.stop();
}());