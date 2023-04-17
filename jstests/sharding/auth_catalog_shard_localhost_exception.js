/**
 * Using the localhost exception, a user can create a cluster wide user on the config server (via
 * the mongos) and a shard specific user on a shard server. On a config shard, since the config
 * server is also a shard server, we want to make sure that we can't use the localhost exception to
 * create two users.
 *
 * @tags: [requires_fcv_70, featureFlagCatalogShard, featureFlagTransitionToCatalogShard]
 */
(function() {
"use strict";

// Test that we can't create a shard specific user on the config shard if we already created a
// cluster wide user using the localhost exception.
var st = new ShardingTest({
    mongos: 1,
    shards: 1,
    config: 1,
    configShard: true,
    keyFile: 'jstests/libs/key1',
    useHostname: false  // This is required to use the localhost auth exception
});
var adminDB = st.s0.getDB('admin');
assert.commandWorked(adminDB.runCommand({createUser: "admin", pwd: "admin", roles: ["root"]}));
adminDB = st.shard0.getDB('admin');
assert.commandFailedWithCode(adminDB.runCommand({createUser: "joe", pwd: "joe", roles: ["root"]}),
                             ErrorCodes.Unauthorized);
assert(adminDB.auth('admin', 'admin'));
st.stop();

// Test that we can't create another cluster wide user if we already created a shard specific user
// on a config shard using the localhost exception.
var st = new ShardingTest({
    mongos: 1,
    shards: 1,
    config: 1,
    configShard: true,
    keyFile: 'jstests/libs/key1',
    useHostname: false  // This is required to use the localhost auth exception
});
var adminDB = st.shard0.getDB('admin');
assert.commandWorked(adminDB.runCommand({createUser: "admin", pwd: "admin", roles: ["root"]}));
assert(adminDB.auth('admin', 'admin'));
adminDB = st.s0.getDB('admin');
assert.commandFailedWithCode(adminDB.runCommand({createUser: "joe", pwd: "joe", roles: ["root"]}),
                             ErrorCodes.Unauthorized);

// Test that the shard specific user created on the config shard is also a cluster wide user by
// using it to auth into the mongos
assert(adminDB.auth('admin', 'admin'));

st.stop();
})();
