/**
 * Test that setShardVersion fails if sent to the config server.
 */
(function() {
"use strict";

var st = new ShardingTest({shards: 1});

var testDB = st.s.getDB('test');
testDB.adminCommand({enableSharding: 'test'});
testDB.adminCommand({shardCollection: 'test.user', key: {x: 1}});

testDB.user.insert({x: 1});

var directConn = new Mongo(st.rs0.getPrimary().host);
var adminDB = directConn.getDB('admin');

var configStr = adminDB.runCommand({getShardVersion: 'test.user'}).configServer;

var configAdmin = st.c0.getDB('admin');

jsTest.log("Verify that setShardVersion fails on the config server");
// Even if shardName sent is 'config' and connstring sent is config server's actual connstring.
assert.commandFailedWithCode(configAdmin.runCommand({
    setShardVersion: '',
    init: true,
    authoritative: true,
    configdb: configStr,
    shard: 'config'
}),
                             ErrorCodes.NoShardingEnabled);

st.stop();
})();
