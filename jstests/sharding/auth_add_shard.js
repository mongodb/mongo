// SERVER-5124
// The puporse of this test is to test authentication when adding/removing a shard. The test sets
// up a sharded system, then adds/removes a shard.
(function() {
    'use strict';

    // login method to login into the database
    function login(userObj) {
        var authResult = merizos.getDB(userObj.db).auth(userObj.username, userObj.password);
        printjson(authResult);
    }

    // admin user object
    var adminUser = {db: "admin", username: "foo", password: "bar"};

    // set up a 2 shard cluster with keyfile
    // TODO: Remove 'shardAsReplicaSet: false' when SERVER-32672 is fixed.
    var st = new ShardingTest(
        {shards: 1, merizos: 1, other: {keyFile: 'jstests/libs/key1', shardAsReplicaSet: false}});

    var merizos = st.s0;
    var admin = merizos.getDB("admin");

    print("1 shard system setup");

    // add the admin user
    print("adding user");
    merizos.getDB(adminUser.db).createUser({
        user: adminUser.username,
        pwd: adminUser.password,
        roles: jsTest.adminUserRoles
    });

    // login as admin user
    login(adminUser);

    assert.eq(1, st.config.shards.count(), "initial server count wrong");

    // start a merizod with NO keyfile
    var conn = MongoRunner.runMongod({shardsvr: ""});
    print(conn);

    // --------------- Test 1 --------------------
    // Add shard to the existing cluster (should fail because it was added without a keyfile)
    printjson(assert.commandFailed(admin.runCommand({addShard: conn.host})));

    // stop merizod
    MongoRunner.stopMongod(conn);

    //--------------- Test 2 --------------------
    // start merizod again, this time with keyfile
    var conn = MongoRunner.runMongod({keyFile: "jstests/libs/key1", shardsvr: ""});
    // try adding the new shard
    assert.commandWorked(admin.runCommand({addShard: conn.host}));

    // Add some data
    var db = merizos.getDB("foo");
    var collA = merizos.getCollection("foo.bar");

    // enable sharding on a collection
    assert.commandWorked(admin.runCommand({enableSharding: "" + collA.getDB()}));
    st.ensurePrimaryShard("foo", "shard0000");

    assert.commandWorked(admin.runCommand({shardCollection: "" + collA, key: {_id: 1}}));

    // add data to the sharded collection
    for (var i = 0; i < 4; i++) {
        db.bar.save({_id: i});
        assert.commandWorked(admin.runCommand({split: "" + collA, middle: {_id: i}}));
    }

    // move a chunk
    assert.commandWorked(admin.runCommand({moveChunk: "foo.bar", find: {_id: 1}, to: "shard0001"}));

    // verify the chunk was moved
    admin.runCommand({flushRouterConfig: 1});

    var config = merizos.getDB("config");
    st.printShardingStatus(true);

    // start balancer before removing the shard
    st.startBalancer();

    //--------------- Test 3 --------------------
    // now drain the shard
    assert.commandWorked(admin.runCommand({removeShard: conn.host}));

    // give it some time to drain
    assert.soon(function() {
        var result = admin.runCommand({removeShard: conn.host});
        printjson(result);

        return result.ok && result.state == "completed";
    }, "failed to drain shard completely", 5 * 60 * 1000);

    assert.eq(1, st.config.shards.count(), "removed server still appears in count");

    MongoRunner.stopMongod(conn);

    st.stop();
})();
