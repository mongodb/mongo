/**
 * Tests that only the internal user will be able to advance the config server opTime.
 */

// The index consistency checker doesn't take into account that
// authentication is needed for contacting shard0 of this cluster
TestData.skipCheckingIndexesConsistentAcrossCluster = true;

(function() {

"use strict";

function getConfigOpTime() {
    var srvStatus = assert.commandWorked(shardTestDB.serverStatus());
    assert.hasFields(srvStatus, ['sharding']);
    return srvStatus.sharding.lastSeenConfigServerOpTime.ts;
}

var st = new ShardingTest({shards: 1, other: {keyFile: 'jstests/libs/key1'}});

const mongosAdminDB = st.s.getDB('admin');
mongosAdminDB.createUser({user: 'foo', pwd: 'bar', roles: jsTest.adminUserRoles});
mongosAdminDB.auth('foo', 'bar');

st.adminCommand({enableSharding: 'test'});
st.adminCommand({shardCollection: 'test.user', key: {x: 1}});

const shardAdminDB = st.rs0.getPrimary().getDB('admin');
const shardTestDB = st.rs0.getPrimary().getDB('test');

// ConfigOpTime can't be advanced from external clients
if (TestData.configShard) {
    // We've already used up the localhost bypass in config shard mode, so we have to log in to
    // create the user below.
    shardAdminDB.auth('foo', 'bar');
}
shardAdminDB.createUser({user: 'user', pwd: 'pwd', roles: jsTest.adminUserRoles});
if (TestData.configShard) {
    shardAdminDB.logout();
}
shardAdminDB.auth('user', 'pwd');
const newTimestamp = Timestamp(getConfigOpTime().getTime() + 1000, 0);
assert.commandWorked(shardTestDB.runCommand({ping: 1, $configTime: newTimestamp}));
assert(timestampCmp(getConfigOpTime(), newTimestamp) < 0, "Unexpected ConfigOpTime advancement");

shardAdminDB.createUser({user: 'internal', pwd: 'pwd', roles: ['__system']});
shardAdminDB.auth('internal', 'pwd');
assert.commandWorked(shardTestDB.runCommand({ping: 1, $configTime: newTimestamp}));
assert(timestampCmp(getConfigOpTime(), newTimestamp) < 0, "Unexpected ConfigOpTime advancement");

mongosAdminDB.logout();
shardAdminDB.logout();
st.stop();
})();
