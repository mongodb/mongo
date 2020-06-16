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
    return srvStatus.sharding.lastSeenConfigServerOpTime.t;
}

var st = new ShardingTest({shards: 1, other: {keyFile: 'jstests/libs/key1'}});

const mongosAdminDB = st.s.getDB('admin');
mongosAdminDB.createUser({user: 'foo', pwd: 'bar', roles: jsTest.adminUserRoles});
mongosAdminDB.auth('foo', 'bar');

st.adminCommand({enableSharding: 'test'});
st.adminCommand({shardCollection: 'test.user', key: {x: 1}});

const shardAdminDB = st.rs0.getPrimary().getDB('admin');
const shardTestDB = st.rs0.getPrimary().getDB('test');
const maxSecs = Math.pow(2, 32) - 1;
const metadata = {
    $configServerState: {opTime: {ts: Timestamp(maxSecs, 0), t: maxSecs}}
};

// ConfigOpTime can't be advanced without the correct permissions
shardAdminDB.createUser({user: 'user', pwd: 'pwd', roles: jsTest.adminUserRoles});
shardAdminDB.auth('user', 'pwd');
var res = shardTestDB.runCommandWithMetadata({ping: 1}, metadata);
assert.commandFailedWithCode(res.commandReply, ErrorCodes.Unauthorized);
assert.lt(getConfigOpTime(), maxSecs, "Unexpected ConfigOpTime advancement");

// Advance configOpTime
shardAdminDB.createUser({user: 'internal', pwd: 'pwd', roles: ['__system']});
shardAdminDB.auth('internal', 'pwd');
res = shardTestDB.runCommandWithMetadata({ping: 1}, metadata);
assert.commandWorked(res.commandReply);
assert.eq(getConfigOpTime(), maxSecs, "ConfigOpTime did not advanced as expected");

mongosAdminDB.logout();
shardAdminDB.logout();
st.stop();
})();
