/**
 * Tests that direct shard connections are correctly allowed and disallowed using authentication.
 *
 * @tags: [featureFlagCheckForDirectShardOperations, requires_fcv_70]
 */
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";

// Create a new sharded cluster for testing and enable auth.
const st = new ShardingTest({name: jsTestName(), keyFile: "jstests/libs/key1", shards: 1});

const shardConn = st.rs0.getPrimary();
const shardAdminDB = shardConn.getDB("admin");
const shardAdminTestDB = shardConn.getDB("test");
const userConn = new Mongo(st.shard0.host);
const userTestDB = userConn.getDB("test");

shardAdminDB.createUser({user: "admin", pwd: 'x', roles: ["root"]});
assert(shardAdminDB.auth("admin", 'x'), "Authentication failed");

// TODO: (SERVER-87190) Remove once 8.0 becomes last lts.
const failOnDirectOps =
    FeatureFlagUtil.isPresentAndEnabled(shardAdminDB, "FailOnDirectShardOperations");

function getUnauthorizedDirectWritesCount() {
    return assert.commandWorked(shardAdminDB.runCommand({serverStatus: 1}))
        .shardingStatistics.unauthorizedDirectShardOps;
}

// With only one shard, direct shard operations should be allowed.
jsTest.log("Running tests with only one shard.");
{
    // Direct writes to collections with root priviledge should be authorized.
    assert.commandWorked(shardAdminTestDB.getCollection("coll").insert({value: 1}));
    assert.eq(getUnauthorizedDirectWritesCount(), 0);

    // Direct writes to collections with read/write priviledge should be authorized.
    shardAdminTestDB.createUser({user: "user", pwd: "y", roles: ["readWrite"]});
    assert(userTestDB.auth("user", "y"), "Authentication failed");
    assert.commandWorked(userTestDB.getCollection("coll").insert({value: 2}));
    assert.eq(getUnauthorizedDirectWritesCount(), 0);

    // Logging out and dropping users should be authorized.
    userTestDB.logout();
    shardAdminTestDB.dropUser("user");
    assert.eq(getUnauthorizedDirectWritesCount(), 0);
}

// Adding the second shard will trigger the check for direct shard ops.
var newShard = new ReplSetTest({name: "additionalShard", nodes: 1});
newShard.startSet({keyFile: "jstests/libs/key1", shardsvr: ""});
newShard.initiate();
let mongosAdminUser = st.s.getDB('admin');
if (!TestData.configShard) {
    mongosAdminUser.createUser({user: "globalAdmin", pwd: 'a', roles: ["root"]});
    assert(mongosAdminUser.auth("globalAdmin", "a"), "Authentication failed");
} else {
    assert(mongosAdminUser.auth("admin", "x"), "Authentication failed");
}
assert.commandWorked(mongosAdminUser.runCommand({addShard: newShard.getURL()}));

jsTest.log("Running tests with two shards.");
{
    // Direct writes to collections with root priviledge (which includes directShardOperations)
    // should be authorized.
    assert.commandWorked(shardAdminTestDB.getCollection("coll").insert({value: 3}));
    assert.eq(getUnauthorizedDirectWritesCount(), 0);

    // Direct writes to collections with read/write priviledge should not be authorized.
    shardAdminTestDB.createUser({user: "user", pwd: "y", roles: ["readWrite"]});
    assert(userTestDB.auth("user", "y"), "Authentication failed");
    if (failOnDirectOps) {
        assert.commandFailedWithCode(userTestDB.getCollection("coll").insert({value: 4}),
                                     ErrorCodes.Unauthorized);
    } else {
        assert.commandWorked(userTestDB.getCollection("coll").insert({value: 4}));
    }
    assert.eq(getUnauthorizedDirectWritesCount(), 1);
    userTestDB.logout();
    assert.eq(getUnauthorizedDirectWritesCount(), 1);

    // Direct writes with just read/write and direct operations should be authorized.
    shardAdminDB.createUser(
        {user: "user2", pwd: "z", roles: ["readWriteAnyDatabase", "directShardOperations"]});
    let shardUserWithDirectWritesAdminDB = userConn.getDB("admin");
    let shardUserWithDirectWritesTestDB = userConn.getDB("test");
    assert(shardUserWithDirectWritesAdminDB.auth("user2", "z"), "Authentication failed");
    assert.commandWorked(shardUserWithDirectWritesTestDB.getCollection("coll").insert({value: 5}));
    assert.eq(getUnauthorizedDirectWritesCount(), 1);

    // Logout should always be authorized and drop user from admin should be authorized.
    shardUserWithDirectWritesAdminDB.logout();
    shardAdminTestDB.dropUser("user");
    shardAdminTestDB.dropUser("user2");
    mongosAdminUser.logout();
    assert.eq(getUnauthorizedDirectWritesCount(), 1);
    // shardAdminDB is used to check the direct writes count, so log it out last.
    shardAdminDB.logout();
}

// Stop the sharding test before the additional shard to ensure the test hooks run successfully.
st.stop();
newShard.stopSet();
