// SERVER-5124
// The puporse of this test is to test authentication when adding/removing a shard. The test sets
// up a sharded system, then adds/removes a shard.
(function() {
'use strict';

// login method to login into the database
function login(userObj) {
    var authResult = mongos.getDB(userObj.db).auth(userObj.username, userObj.password);
    printjson(authResult);
}

// admin user object
var adminUser = {db: "admin", username: "foo", password: "bar"};

// set up a 2 shard cluster with keyfile
var st = new ShardingTest({shards: 1, mongos: 1, other: {keyFile: 'jstests/libs/key1'}});

var mongos = st.s0;
var admin = mongos.getDB("admin");

print("1 shard system setup");

// add the admin user
print("adding user");
mongos.getDB(adminUser.db)
    .createUser({user: adminUser.username, pwd: adminUser.password, roles: jsTest.adminUserRoles});

// login as admin user
login(adminUser);

assert.eq(1, st.config.shards.count(), "initial server count wrong");

// start a mongod with NO keyfile
var rst = new ReplSetTest({nodes: 1});
rst.startSet({shardsvr: ""});
rst.initiateWithAnyNodeAsPrimary();

// --------------- Test 1 --------------------
// Add shard to the existing cluster (should fail because it was added without a keyfile)
printjson(assert.commandFailed(admin.runCommand({addShard: rst.getURL()})));

// stop mongod
rst.stopSet();

//--------------- Test 2 --------------------
// start mongod again, this time with keyfile
rst = new ReplSetTest({nodes: 1});
rst.startSet({keyFile: "jstests/libs/key1", shardsvr: ""});
rst.initiateWithAnyNodeAsPrimary();

// try adding the new shard
var addShardRes = admin.runCommand({addShard: rst.getURL()});
assert.commandWorked(addShardRes);

// Add some data
var db = mongos.getDB("foo");
var collA = mongos.getCollection("foo.bar");

// enable sharding on a collection
assert.commandWorked(admin.runCommand({enableSharding: "" + collA.getDB()}));
st.ensurePrimaryShard("foo", st.shard0.shardName);

assert.commandWorked(admin.runCommand({shardCollection: "" + collA, key: {_id: 1}}));

// add data to the sharded collection
for (var i = 0; i < 4; i++) {
    db.bar.save({_id: i});
    assert.commandWorked(admin.runCommand({split: "" + collA, middle: {_id: i}}));
}

// move a chunk
assert.commandWorked(
    admin.runCommand({moveChunk: "foo.bar", find: {_id: 1}, to: addShardRes.shardAdded}));

// verify the chunk was moved
admin.runCommand({flushRouterConfig: 1});

var config = mongos.getDB("config");
st.printShardingStatus(true);

// start balancer before removing the shard
st.startBalancer();

//--------------- Test 3 --------------------
// now drain the shard
assert.commandWorked(admin.runCommand({removeShard: rst.getURL()}));

// give it some time to drain
assert.soon(function() {
    var result = admin.runCommand({removeShard: rst.getURL()});
    printjson(result);

    return result.ok && result.state == "completed";
}, "failed to drain shard completely", 5 * 60 * 1000);

assert.eq(1, st.config.shards.count(), "removed server still appears in count");

rst.stopSet();

st.stop();
})();
