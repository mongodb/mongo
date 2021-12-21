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

// ConfigOpTime can't be advanced without the correct permissions
shardAdminDB.createUser({user: 'user', pwd: 'pwd', roles: jsTest.adminUserRoles});
shardAdminDB.auth('user', 'pwd');
const newTimestamp = Timestamp(getConfigOpTime().getTime() + 1000, 0);
assert.commandFailedWithCode(
    shardTestDB.runCommand({ping: 1, $configServerState: {opTime: {ts: newTimestamp, t: -1}}}),
    ErrorCodes.Unauthorized);
assert(timestampCmp(getConfigOpTime(), newTimestamp) < 0, "Unexpected ConfigOpTime advancement");

// Advance configOpTime
shardAdminDB.createUser({user: 'internal', pwd: 'pwd', roles: ['__system']});
shardAdminDB.auth('internal', 'pwd');
assert.commandWorked(
    shardTestDB.runCommand({ping: 1, $configServerState: {opTime: {ts: newTimestamp, t: -1}}}));
assert(timestampCmp(getConfigOpTime(), newTimestamp) >= 0,
       "ConfigOpTime did not advanced as expected");

mongosAdminDB.logout();
shardAdminDB.logout();
st.stop();
})();
