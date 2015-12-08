(function() {
"use strict";

var st = new ShardingTest({ shards: 1, other: { sync: true }});

var testDB = st.s.getDB('test');
testDB.adminCommand({ enableSharding: 'test' });
testDB.adminCommand({ shardCollection: 'test.user', key: { x: 1 }});

// Initialize version on shard.
testDB.user.insert({ x: 1 });

var directConn = new Mongo(st.d0.host);
var adminDB = directConn.getDB('admin');

var configStr = adminDB.runCommand({ getShardVersion: 'test.user' }).configServer;
var configStrArr = configStr.split(',');
assert.eq(3, configStrArr.length);

var badConfigStr = configStrArr[1] + ',' + configStrArr[2] + ',' + configStrArr[0];

var configAdmin = st.c0.getDB('admin');

// Initialize internal config string.
assert.commandWorked(configAdmin.runCommand({
    setShardVersion: '',
    init: true,
    authoritative: true,
    configdb: configStr,
    shard: 'config'
}));

// Passing configdb that does not match initialized value is not ok.
assert.commandFailed(configAdmin.runCommand({
    setShardVersion: '',
    init: true,
    authoritative: true,
    configdb: badConfigStr,
    shard: 'config'
}));

// Passing configdb that matches initialized value is ok.
assert.commandWorked(configAdmin.runCommand({
    setShardVersion: '',
    init: true,
    authoritative: true,
    configdb: configStr,
    shard: 'config'
}));

st.stop();

})();
