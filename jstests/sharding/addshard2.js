/**
 * Tests adding standalones and replica sets as shards under a variety of configurations (setName,
 * valid and invalid hosts, shardName matching or not matching a setName, etc).
 */
(function() {

    var addShardRes;
    var rst;

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

    // Enable the failpoint that prevents the config server from upserting a shardIdentity on new
    // shards so that the same shard host can be re-used for multiple addShard calls without being
    // restarted in between each addShard (the shardIdentity cannot be deleted while the shard host
    // is running with --shardsvr).
    // Note: such a failpoint is not necessary for the addShard compatibility path for 3.2 (for the
    // last_stable suite), since a 3.2 mongos will ignore the shard's shardIdentity during addShard.
    var st = new ShardingTest({
        shards: 0,
        mongos: 1,
        other: {
            configOptions: {
                setParameter: "failpoint.dontUpsertShardIdentityOnNewShards={'mode':'alwaysOn'}"
            }
        }
    });

    // Add one shard since the last shard cannot be removed.
    var normalShard = MongoRunner.runMongod({shardsvr: ''});
    st.s.adminCommand({addShard: normalShard.name, name: 'normalShard'});

    // Allocate a port that can be used to test adding invalid hosts.
    var portWithoutHostRunning = allocatePort();

    // 1. Test adding a *standalone*

    // 1.a. with or without specifying the shardName.

    var standalone = MongoRunner.runMongod({shardsvr: ''});

    jsTest.log("Adding a standalone *without* a specified shardName should succeed.");
    addShardRes = st.s.adminCommand({addshard: standalone.name});
    assertAddShardSucceeded(addShardRes);
    removeShardWithName(addShardRes.shardAdded);

    jsTest.log("Adding a standalone *with* a specified shardName should succeed.");
    addShardRes = st.s.adminCommand({addshard: standalone.name, name: "shardName"});
    assertAddShardSucceeded(addShardRes, "shardName");
    removeShardWithName(addShardRes.shardAdded);

    MongoRunner.stopMongod(standalone);

    // 1.b. with an invalid hostname.

    jsTest.log("Adding a standalone with a non-existing host should fail.");
    addShardRes = st.s.adminCommand({addShard: getHostName() + ":" + portWithoutHostRunning});
    assertAddShardFailed(addShardRes);

    // 2. Test adding a *replica set* with an ordinary set name

    // 2.a. with or without specifying the shardName.

    rst = new ReplSetTest({nodes: 1});
    rst.startSet({shardsvr: ''});
    rst.initiate();

    jsTest.log("Adding a replica set without a specified shardName should succeed.");
    addShardRes = st.s.adminCommand({addShard: rst.getURL()});
    assertAddShardSucceeded(addShardRes);
    assert.eq(rst.name, addShardRes.shardAdded);
    removeShardWithName(addShardRes.shardAdded);

    jsTest.log(
        "Adding a replica set with a specified shardName that matches the set's name should succeed.");
    addShardRes = st.s.adminCommand({addShard: rst.getURL(), name: rst.name});
    assertAddShardSucceeded(addShardRes, rst.name);
    removeShardWithName(addShardRes.shardAdded);

    jsTest.log(
        "Adding a replica set with a specified shardName that differs from the set's name should succeed.");
    addShardRes = st.s.adminCommand({addShard: rst.getURL(), name: "differentShardName"});
    assertAddShardSucceeded(addShardRes, "differentShardName");
    removeShardWithName(addShardRes.shardAdded);

    jsTest.log("Adding a replica with a specified shardName of 'config' should fail.");
    addShardRes = st.s.adminCommand({addShard: rst.getURL(), name: "config"});
    assertAddShardFailed(addShardRes, "config");

    // 2.b. with invalid hostnames.

    jsTest.log("Adding a replica set with only non-existing hosts should fail.");
    addShardRes =
        st.s.adminCommand({addShard: rst.name + "/NonExistingHost:" + portWithoutHostRunning});
    assertAddShardFailed(addShardRes);

    jsTest.log("Adding a replica set with mixed existing/non-existing hosts should fail.");
    addShardRes = st.s.adminCommand({
        addShard:
            rst.name + "/" + rst.getPrimary().name + ",NonExistingHost:" + portWithoutHostRunning
    });
    assertAddShardFailed(addShardRes);

    rst.stopSet();

    // 3. Test adding a replica set whose *set name* is "config" with or without specifying the
    // shardName.

    rst = new ReplSetTest({name: "config", nodes: 1});
    rst.startSet({shardsvr: ''});
    rst.initiate();

    jsTest.log(
        "Adding a replica set whose setName is config without specifying shardName should fail.");
    addShardRes = st.s.adminCommand({addShard: rst.getURL()});
    assertAddShardFailed(addShardRes);

    jsTest.log(
        "Adding a replica set whose setName is config with specified shardName 'config' should fail.");
    addShardRes = st.s.adminCommand({addShard: rst.getURL(), name: rst.name});
    assertAddShardFailed(addShardRes, rst.name);

    jsTest.log(
        "Adding a replica set whose setName is config with a non-'config' shardName should succeed");
    addShardRes = st.s.adminCommand({addShard: rst.getURL(), name: "nonConfig"});
    assertAddShardSucceeded(addShardRes, "nonConfig");
    removeShardWithName(addShardRes.shardAdded);

    rst.stopSet();

    // 4. Test that a replica set whose *set name* is "admin" can be written to (SERVER-17232).

    rst = new ReplSetTest({name: "admin", nodes: 1});
    rst.startSet({shardsvr: ''});
    rst.initiate();

    jsTest.log("A replica set whose set name is 'admin' should be able to be written to.");
    addShardRes = st.s.adminCommand({addShard: rst.getURL()});
    assertAddShardSucceeded(addShardRes);
    assert.writeOK(st.s.getDB('test').foo.insert({x: 1}));
    assert.commandWorked(st.s.getDB('test').runCommand({dropDatabase: 1}));
    removeShardWithName(addShardRes.shardAdded);

    rst.stopSet();

    st.stop();

})();
