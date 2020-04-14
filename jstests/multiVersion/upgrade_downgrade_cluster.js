/**
 * Tests that CRUD and aggregation commands through the mongos continue to work as expected on both
 * sharded and unsharded collection at each step of cluster upgrade from last-stable to latest.
 */
(function() {
"use strict";

load('./jstests/multiVersion/libs/multi_rs.js');
load('./jstests/multiVersion/libs/multi_cluster.js');

// When checking UUID consistency, the shell attempts to run a command on the node it believes is
// primary in each shard. However, this test restarts shards, and the node that is elected primary
// after the restart may be different from the original primary. Since the shell does not retry on
// NotMaster errors, and whether or not it detects the new primary before issuing the command is
// nondeterministic, skip the consistency check for this test.
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;

const kMinVersion = 5;
const kCurrentVerion = 6;

var testCRUDAndAgg = function(db) {
    assert.commandWorked(db.foo.insert({x: 1}));
    assert.commandWorked(db.foo.insert({x: -1}));
    assert.commandWorked(db.foo.update({x: 1}, {$set: {y: 1}}));
    assert.commandWorked(db.foo.update({x: -1}, {$set: {y: 1}}));
    var doc1 = db.foo.findOne({x: 1});
    assert.eq(1, doc1.y);
    var doc2 = db.foo.findOne({x: -1});
    assert.eq(1, doc2.y);

    assert.commandWorked(db.foo.remove({x: 1}, true));
    assert.commandWorked(db.foo.remove({x: -1}, true));
    assert.eq(null, db.foo.findOne());
};

var st = new ShardingTest({
    shards: 2,
    mongos: 1,
    other: {
        mongosOptions: {binVersion: "last-stable"},
        configOptions: {binVersion: "last-stable"},
        shardOptions: {binVersion: "last-stable"},

        rsOptions: {binVersion: "last-stable"},
        rs: true,
    }
});
st.configRS.awaitReplication();

// check that config.version document gets initialized properly
var version = st.s.getCollection('config.version').findOne();
assert.eq(version.minCompatibleVersion, kMinVersion);
assert.eq(version.currentVersion, kCurrentVerion);
var clusterID = version.clusterId;
assert.neq(null, clusterID);
assert.eq(version.excluding, undefined);

// Setup sharded collection
assert.commandWorked(st.s.adminCommand({enableSharding: 'sharded'}));
st.ensurePrimaryShard('sharded', st.shard0.shardName);

assert.commandWorked(st.s.adminCommand({shardCollection: 'sharded.foo', key: {x: 1}}));
assert.commandWorked(st.s.adminCommand({split: 'sharded.foo', middle: {x: 0}}));
assert.commandWorked(
    st.s.adminCommand({moveChunk: 'sharded.foo', find: {x: 1}, to: st.shard1.shardName}));

testCRUDAndAgg(st.s.getDB('unsharded'));
testCRUDAndAgg(st.s.getDB('sharded'));

// upgrade the config servers first
jsTest.log('upgrading config servers');
st.upgradeCluster("latest", {upgradeMongos: false, upgradeShards: false});

testCRUDAndAgg(st.s.getDB('unsharded'));
testCRUDAndAgg(st.s.getDB('sharded'));

// Restart mongos to clear all cache and force it to do remote calls.
st.restartMongoses();

testCRUDAndAgg(st.s.getDB('unsharded'));
testCRUDAndAgg(st.s.getDB('sharded'));

// Then upgrade the shards.
jsTest.log('upgrading shard servers');
st.upgradeCluster("latest", {upgradeMongos: false, upgradeConfigs: false});

awaitRSClientHosts(st.s, st.rs0.getPrimary(), {ok: true, ismaster: true});
awaitRSClientHosts(st.s, st.rs1.getPrimary(), {ok: true, ismaster: true});

testCRUDAndAgg(st.s.getDB('unsharded'));
testCRUDAndAgg(st.s.getDB('sharded'));

// Restart mongos to clear all cache and force it to do remote calls.
st.restartMongoses();

testCRUDAndAgg(st.s.getDB('unsharded'));
testCRUDAndAgg(st.s.getDB('sharded'));

// Finally, upgrade mongos
jsTest.log('upgrading mongos servers');
st.upgradeCluster("latest", {upgradeConfigs: false, upgradeShards: false});

testCRUDAndAgg(st.s.getDB('unsharded'));
testCRUDAndAgg(st.s.getDB('sharded'));

// Restart mongos to clear all cache and force it to do remote calls.
st.restartMongoses();

testCRUDAndAgg(st.s.getDB('unsharded'));
testCRUDAndAgg(st.s.getDB('sharded'));

// Check that version document is unmodified.
version = st.s.getCollection('config.version').findOne();
assert.eq(version.minCompatibleVersion, kMinVersion);
assert.eq(version.currentVersion, kCurrentVerion);
assert.eq(clusterID, version.clusterId);
assert.eq(version.excluding, undefined);

///////////////////////////////////////////////////////////////////////////////////////////
// Downgrade back

jsTest.log('downgrading mongos servers');
st.upgradeCluster("last-stable", {upgradeConfigs: false, upgradeShards: false});

testCRUDAndAgg(st.s.getDB('unsharded'));
testCRUDAndAgg(st.s.getDB('sharded'));

// Restart mongos to clear all cache and force it to do remote calls.
st.restartMongoses();

testCRUDAndAgg(st.s.getDB('unsharded'));
testCRUDAndAgg(st.s.getDB('sharded'));

jsTest.log('downgrading shard servers');
st.upgradeCluster("last-stable", {upgradeMongos: false, upgradeConfigs: false});

awaitRSClientHosts(st.s, st.rs0.getPrimary(), {ok: true, ismaster: true});
awaitRSClientHosts(st.s, st.rs1.getPrimary(), {ok: true, ismaster: true});

testCRUDAndAgg(st.s.getDB('unsharded'));
testCRUDAndAgg(st.s.getDB('sharded'));

// Restart mongos to clear all cache and force it to do remote calls.
st.restartMongoses();

testCRUDAndAgg(st.s.getDB('unsharded'));
testCRUDAndAgg(st.s.getDB('sharded'));

jsTest.log('downgrading config servers');
st.upgradeCluster("last-stable", {upgradeMongos: false, upgradeShards: false});

testCRUDAndAgg(st.s.getDB('unsharded'));
testCRUDAndAgg(st.s.getDB('sharded'));

// Restart mongos to clear all cache and force it to do remote calls.
st.restartMongoses();

testCRUDAndAgg(st.s.getDB('unsharded'));
testCRUDAndAgg(st.s.getDB('sharded'));

// Check that version document is unmodified.
version = st.s.getCollection('config.version').findOne();
assert.eq(version.minCompatibleVersion, kMinVersion);
assert.eq(version.currentVersion, kCurrentVerion);
assert.eq(clusterID, version.clusterId);
assert.eq(version.excluding, undefined);

st.stop();
})();
