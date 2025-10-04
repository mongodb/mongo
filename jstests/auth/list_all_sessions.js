// Auth tests for the $listSessions {allUsers:true} aggregation stage.
// @tags: [requires_sharding, requires_auth]

import {assertErrorCode} from "jstests/aggregation/extras/utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

// This test makes assertions about the number of sessions, which are not compatible with
// implicit sessions.
TestData.disableImplicitSessions = true;

function runListAllSessionsTest(mongod) {
    assert(mongod);
    const admin = mongod.getDB("admin");
    const config = mongod.getDB("config");

    const pipeline = [{"$listSessions": {allUsers: true}}];

    admin.createUser({user: "admin", pwd: "pass", roles: jsTest.adminUserRoles});
    assert(admin.auth("admin", "pass"));
    admin.createUser({user: "user1", pwd: "pass", roles: jsTest.basicUserRoles});
    admin.logout();

    // Fail if we're not logged in.
    assertErrorCode(config.system.sessions, pipeline, ErrorCodes.Unauthorized);

    // Start a new session and capture its sessionId.
    assert(admin.auth("user1", "pass"));
    const myid = assert.commandWorked(admin.runCommand({startSession: 1})).id.id;
    assert(myid !== undefined);
    assert.commandWorked(admin.runCommand({refreshLogicalSessionCacheNow: 1}));

    // Ensure that a normal user can NOT listSessions{allUsers:true} to view their session.
    assertErrorCode(config.system.sessions, pipeline, ErrorCodes.Unauthorized);

    // Ensure that a normal user can NOT listSessions to view others' sessions.
    const viewAdminPipeline = [{"$listSessions": {users: [{user: "admin", db: "admin"}]}}];
    assertErrorCode(config.system.sessions, viewAdminPipeline, ErrorCodes.Unauthorized);

    // Ensure that the cache now contains the session and is visible by admin
    admin.logout();
    assert(admin.auth("admin", "pass"));
    const resultArray = config.system.sessions
        .aggregate([{"$listSessions": {allUsers: true}}])
        .toArray()
        .filter((session) => {
            assert(session._id.id !== undefined);
            return 0 == bsonWoCompare({y: myid}, {y: session._id.id});
        });
    assert.eq(resultArray.length, 1);

    // Make sure pipelining other collections fail.
    assertErrorCode(admin.system.collections, pipeline, ErrorCodes.InvalidNamespace);
}

const mongod = MongoRunner.runMongod({auth: ""});
runListAllSessionsTest(mongod);
MongoRunner.stopMongod(mongod);

const st = new ShardingTest({shards: 1, mongos: 1, config: 1, other: {keyFile: "jstests/libs/key1"}});

// Ensure that the sessions collection exists.
st.c0.getDB("admin").runCommand({refreshLogicalSessionCacheNow: 1});
st.rs0.getPrimary().getDB("admin").runCommand({refreshLogicalSessionCacheNow: 1});

runListAllSessionsTest(st.s0);
st.stop();
