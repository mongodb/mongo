//
// Tests of cleanupOrphaned command permissions.
//

import {ShardingTest} from "jstests/libs/shardingtest.js";

// Multiple users cannot be authenticated on one connection within a session.
TestData.disableImplicitSessions = true;

function assertUnauthorized(res, msg) {
    if (res.ok == 0 && (res.errmsg.startsWith("not authorized") || res.errmsg.match(/requires authentication/))) return;

    let finalMsg = "command worked when it should have been unauthorized: " + tojson(res);
    if (msg) {
        finalMsg += " : " + msg;
    }
    doassert(finalMsg);
}

let st = new ShardingTest({auth: true, other: {keyFile: "jstests/libs/key1", useHostname: false}});

let shardAdmin = st.shard0.getDB("admin");
if (!TestData.configShard) {
    // In config shard mode, this will create a user on the config server, which we already do
    // below.
    shardAdmin.createUser({
        user: "admin",
        pwd: "x",
        roles: ["clusterAdmin", "userAdminAnyDatabase", "directShardOperations"],
    });
    shardAdmin.auth("admin", "x");
}

let mongos = st.s0;
let mongosAdmin = mongos.getDB("admin");
let coll = mongos.getCollection("foo.bar");

mongosAdmin.createUser({
    user: "admin",
    pwd: "x",
    roles: ["clusterAdmin", "userAdminAnyDatabase", "directShardOperations"],
});
mongosAdmin.auth("admin", "x");

assert.commandWorked(mongosAdmin.runCommand({enableSharding: coll.getDB().getName()}));

assert.commandWorked(mongosAdmin.runCommand({shardCollection: coll.getFullName(), key: {_id: "hashed"}}));

// cleanupOrphaned requires auth as admin user.
if (!TestData.configShard) {
    assert.commandWorked(shardAdmin.logout());
}
assertUnauthorized(shardAdmin.runCommand({cleanupOrphaned: "foo.bar"}));

let fooDB = st.shard0.getDB("foo");
shardAdmin.auth("admin", "x");
fooDB.createUser({user: "user", pwd: "x", roles: ["readWrite", "dbAdmin"]});
shardAdmin.logout();
fooDB.auth("user", "x");
assertUnauthorized(shardAdmin.runCommand({cleanupOrphaned: "foo.bar"}));

st.stop();
