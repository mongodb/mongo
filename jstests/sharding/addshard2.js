/**
 * Tests adding standalones and replica sets as shards under a variety of configurations (setName,
 * valid and invalid hosts, shardName matching or not matching a setName, etc).
 */
(function() {

    var addShardRes;

    var assertAddShardSucceeded = function(res, shardName) {
        assert.commandWorked(res);

        // If a shard name was specified, make sure that the name the addShard command reports the
        // shard was added with matches the specified name.
        if (shardName) {
            assert.eq(shardName,
                      res.shardAdded,
                      "name returned by addShard does not match name specified in addShard");
        }

        // Make sure the shard shows up in config.shards with the shardName reported by the
        // addShard command.
        assert.neq(null,
                   st.s.getDB('config').shards.findOne({_id: res.shardAdded}),
                   "newly added shard " + res.shardAdded + " not found in config.shards");
    };

    // Note: this method expects that the failure is *not* that the specified shardName is already
    // the shardName of an existing shard.
    var assertAddShardFailed = function(res, shardName) {
        assert.commandFailed(res);

        // If a shard name was specified in the addShard, make sure no shard with its name shows up
        // in config.shards.
        if (shardName) {
            assert.eq(null,
                      st.s.getDB('config').shards.findOne({_id: shardName}),
                      "addShard for " + shardName +
                          " reported failure, but shard shows up in config.shards");
        }
    };

    var removeShardWithName = function(shardName) {
        var res = st.s.adminCommand({removeShard: shardName});
        assert.commandWorked(res);
        assert.eq('started', res.state);
        assert.soon(function() {
            res = st.s.adminCommand({removeShard: shardName});
            assert.commandWorked(res);
            return ('completed' === res.state);
        }, "removeShard never completed for shard " + shardName);
    };

    var st = new ShardingTest({
        shards: 0,
        mongos: 1,
    });

    // Add one shard since the last shard cannot be removed.
    var normalShard = MongoRunner.runMongod({shardsvr: ''});
    st.s.adminCommand({addShard: normalShard.name, name: 'normalShard'});

    // Allocate a port that can be used to test adding invalid hosts.
    var portWithoutHostRunning = allocatePort();

    // 1. Test adding a *standalone*

    // 1.a. with or without specifying the shardName.

    jsTest.log("Adding a standalone *without* a specified shardName should succeed.");
    let standalone1 = MongoRunner.runMongod({shardsvr: ''});
    addShardRes = st.s.adminCommand({addshard: standalone1.name});
    assertAddShardSucceeded(addShardRes);
    removeShardWithName(addShardRes.shardAdded);
    MongoRunner.stopMongod(standalone1);

    jsTest.log("Adding a standalone *with* a specified shardName should succeed.");
    let standalone2 = MongoRunner.runMongod({shardsvr: ''});
    addShardRes = st.s.adminCommand({addshard: standalone2.name, name: "shardName"});
    assertAddShardSucceeded(addShardRes, "shardName");
    removeShardWithName(addShardRes.shardAdded);
    MongoRunner.stopMongod(standalone2);

    // 1.b. with an invalid hostname.

    jsTest.log("Adding a standalone with a non-existing host should fail.");
    addShardRes = st.s.adminCommand({addShard: getHostName() + ":" + portWithoutHostRunning});
    assertAddShardFailed(addShardRes);

    // 2. Test adding a *replica set* with an ordinary set name

    // 2.a. with or without specifying the shardName.

    jsTest.log("Adding a replica set without a specified shardName should succeed.");
    let rst1 = new ReplSetTest({nodes: 1});
    rst1.startSet({shardsvr: ''});
    rst1.initiate();
    addShardRes = st.s.adminCommand({addShard: rst1.getURL()});
    assertAddShardSucceeded(addShardRes);
    assert.eq(rst1.name, addShardRes.shardAdded);
    removeShardWithName(addShardRes.shardAdded);
    rst1.stopSet();

    jsTest.log(
        "Adding a replica set with a specified shardName that matches the set's name should succeed.");
    let rst2 = new ReplSetTest({nodes: 1});
    rst2.startSet({shardsvr: ''});
    rst2.initiate();
    addShardRes = st.s.adminCommand({addShard: rst2.getURL(), name: rst2.name});
    assertAddShardSucceeded(addShardRes, rst2.name);
    removeShardWithName(addShardRes.shardAdded);
    rst2.stopSet();

    let rst3 = new ReplSetTest({nodes: 1});
    rst3.startSet({shardsvr: ''});
    rst3.initiate();

    jsTest.log(
        "Adding a replica set with a specified shardName that differs from the set's name should succeed.");
    addShardRes = st.s.adminCommand({addShard: rst3.getURL(), name: "differentShardName"});
    assertAddShardSucceeded(addShardRes, "differentShardName");
    removeShardWithName(addShardRes.shardAdded);

    jsTest.log("Adding a replica with a specified shardName of 'config' should fail.");
    addShardRes = st.s.adminCommand({addShard: rst3.getURL(), name: "config"});
    assertAddShardFailed(addShardRes, "config");

    // 2.b. with invalid hostnames.

    jsTest.log("Adding a replica set with only non-existing hosts should fail.");
    addShardRes =
        st.s.adminCommand({addShard: rst3.name + "/NonExistingHost:" + portWithoutHostRunning});
    assertAddShardFailed(addShardRes);

    jsTest.log("Adding a replica set with mixed existing/non-existing hosts should fail.");
    addShardRes = st.s.adminCommand({
        addShard:
            rst3.name + "/" + rst3.getPrimary().name + ",NonExistingHost:" + portWithoutHostRunning
    });
    assertAddShardFailed(addShardRes);

    rst3.stopSet();

    // 3. Test adding a replica set whose *set name* is "config" with or without specifying the
    // shardName.

    let rst4 = new ReplSetTest({name: "config", nodes: 1});
    rst4.startSet({shardsvr: ''});
    rst4.initiate();

    jsTest.log(
        "Adding a replica set whose setName is config without specifying shardName should fail.");
    addShardRes = st.s.adminCommand({addShard: rst4.getURL()});
    assertAddShardFailed(addShardRes);

    jsTest.log(
        "Adding a replica set whose setName is config with specified shardName 'config' should fail.");
    addShardRes = st.s.adminCommand({addShard: rst4.getURL(), name: rst4.name});
    assertAddShardFailed(addShardRes, rst4.name);

    jsTest.log(
        "Adding a replica set whose setName is config with a non-'config' shardName should succeed");
    addShardRes = st.s.adminCommand({addShard: rst4.getURL(), name: "nonConfig"});
    assertAddShardSucceeded(addShardRes, "nonConfig");
    removeShardWithName(addShardRes.shardAdded);

    rst4.stopSet();

    // 4. Test that a replica set whose *set name* is "admin" can be written to (SERVER-17232).

    let rst5 = new ReplSetTest({name: "admin", nodes: 1});
    rst5.startSet({shardsvr: ''});
    rst5.initiate();

    jsTest.log("A replica set whose set name is 'admin' should be able to be written to.");

    addShardRes = st.s.adminCommand({addShard: rst5.getURL()});
    assertAddShardSucceeded(addShardRes);

    // Ensure the write goes to the newly added shard.
    assert.commandWorked(st.s.getDB('test').runCommand({create: "foo"}));
    var res = st.s.getDB('config').getCollection('databases').findOne({_id: 'test'});
    assert.neq(null, res);
    if (res.primary != addShardRes.shardAdded) {
        assert.commandWorked(st.s.adminCommand({movePrimary: 'test', to: addShardRes.shardAdded}));
    }

    assert.writeOK(st.s.getDB('test').foo.insert({x: 1}));
    assert.neq(null, rst5.getPrimary().getDB('test').foo.findOne());

    assert.commandWorked(st.s.getDB('test').runCommand({dropDatabase: 1}));

    removeShardWithName(addShardRes.shardAdded);

    rst5.stopSet();

    st.stop();

})();
