/**
 * Tests adding standalones and replica sets as shards under a variety of configurations (setName,
 * valid and invalid hosts, shardName matching or not matching a setName, etc).
 */
(function() {
'use strict';
load('jstests/sharding/libs/remove_shard_util.js');

// TODO SERVER-50144 Remove this and allow orphan checking.
// This test calls removeShard which can leave docs in config.rangeDeletions in state "pending",
// therefore preventing orphans from being cleaned up.
TestData.skipCheckOrphans = true;

let addShardRes;

const assertAddShardSucceeded = function(res, shardName) {
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
const assertAddShardFailed = function(res, shardName) {
    assert.commandFailed(res);

    // If a shard name was specified in the addShard, make sure no shard with its name shows up
    // in config.shards.
    if (shardName) {
        if (TestData.configShard && shardName === "config") {
            // In config shard mode there's always an entry for config for the config server.
            assert.neq(null, st.s.getDB('config').shards.findOne({_id: shardName}));
        } else {
            assert.eq(null,
                      st.s.getDB('config').shards.findOne({_id: shardName}),
                      "addShard for " + shardName +
                          " reported failure, but shard shows up in config.shards");
        }
    }
};

const st = new ShardingTest({
    shards: TestData.configShard ? 1 : 0,
    mongos: 1,
});

// Add one shard since the last shard cannot be removed.
const normalShard = new ReplSetTest({name: "addshard2-1", nodes: 1, nodeOptions: {shardsvr: ""}});
normalShard.startSet();
normalShard.initiate();

st.s.adminCommand({addShard: normalShard.getURL(), name: 'normalShard'});

// Allocate a port that can be used to test adding invalid hosts.
const portWithoutHostRunning = allocatePort();

// 1. Test adding a *replica set* with an ordinary set name

// 1.a. with or without specifying the shardName.

jsTest.log("Adding a replica set without a specified shardName should succeed.");
const rst1 = new ReplSetTest({nodes: 1});
rst1.startSet({shardsvr: ''});
rst1.initiate();
addShardRes = st.s.adminCommand({addShard: rst1.getURL()});
assertAddShardSucceeded(addShardRes);
assert.eq(rst1.name, addShardRes.shardAdded);
removeShard(st, addShardRes.shardAdded);
rst1.stopSet();

jsTest.log(
    "Adding a replica set with a specified shardName that matches the set's name should succeed.");
const rst2 = new ReplSetTest({nodes: 1});
rst2.startSet({shardsvr: ''});
rst2.initiate();
addShardRes = st.s.adminCommand({addShard: rst2.getURL(), name: rst2.name});
assertAddShardSucceeded(addShardRes, rst2.name);
removeShard(st, addShardRes.shardAdded);
rst2.stopSet();

let rst3 = new ReplSetTest({nodes: 1});
rst3.startSet({shardsvr: ''});
rst3.initiate();

jsTest.log(
    "Adding a replica set with a specified shardName that differs from the set's name should succeed.");
addShardRes = st.s.adminCommand({addShard: rst3.getURL(), name: "differentShardName"});
assertAddShardSucceeded(addShardRes, "differentShardName");
removeShard(st, addShardRes.shardAdded);

jsTest.log("Adding a replica with a specified shardName of 'config' should fail.");
addShardRes = st.s.adminCommand({addShard: rst3.getURL(), name: "config"});
assertAddShardFailed(addShardRes, "config");

// 1.b. with invalid hostnames.

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

// 2. Test adding a replica set whose *set name* is "config" with or without specifying the
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
removeShard(st, addShardRes.shardAdded);

rst4.stopSet();

// 3. Test that a replica set whose *set name* is "admin" can be written to (SERVER-17232).

let rst5 = new ReplSetTest({name: "admin", nodes: 1});
rst5.startSet({shardsvr: ''});
rst5.initiate();

jsTest.log("A replica set whose set name is 'admin' should be able to be written to.");

addShardRes = st.s.adminCommand({addShard: rst5.getURL()});
assertAddShardSucceeded(addShardRes);

// Ensure the write goes to the newly added shard.
assert.commandWorked(st.s.getDB('test').runCommand({create: "foo"}));
const res = st.s.getDB('config').getCollection('databases').findOne({_id: 'test'});
assert.neq(null, res);
if (res.primary != addShardRes.shardAdded) {
    assert.commandWorked(st.s.adminCommand({movePrimary: 'test', to: addShardRes.shardAdded}));
}

assert.commandWorked(st.s.getDB('test').foo.insert({x: 1}));
assert.neq(null, rst5.getPrimary().getDB('test').foo.findOne());

assert.commandWorked(st.s.getDB('test').runCommand({dropDatabase: 1}));

removeShard(st, addShardRes.shardAdded);

rst5.stopSet();

st.stop();
normalShard.stopSet();
})();
