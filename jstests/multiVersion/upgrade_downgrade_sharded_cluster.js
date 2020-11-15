/**
 * This test checks several upgrade/downgrade routines related to sharding, namely:
 *  - The entries in the config server's config.collections that were marked as 'dropped: true' are
 *    deleted. (SERVER-52630: Remove once 5.0 becomes the LastLTS)
 *  - The entries in the config server's config.collections contain a 'distributionMode' field.
 *    (SERVER-51900: Remove once 5.0 becomes the LastLTS)
 */
(function() {
"use strict";

load('./jstests/multiVersion/libs/multi_cluster.js');  // for upgradeCluster()

// Test 1: it checks two things after upgrading from 4.4:
// - dropped collections are not present in csrs config.collections
// - Entries on config.collections doesn't have the 'distributionMode' and the 'dropped' fields
function test1Setup() {
    let csrs_config_db = st.configRS.getPrimary().getDB('config');
    // Setup sharded collections
    assert.commandWorked(st.s.adminCommand({shardCollection: 'sharded.foo', key: {x: 1}}));
    assert.commandWorked(st.s.adminCommand({shardCollection: 'sharded.bar', key: {x: 1}}));

    {
        var collFoo = csrs_config_db.collections.findOne({_id: 'sharded.foo'});
        assert.eq('sharded', collFoo.distributionMode);
        assert.eq(false, collFoo.dropped);

        var collBar = csrs_config_db.collections.findOne({_id: 'sharded.bar'});
        assert.eq('sharded', collBar.distributionMode);
        assert.eq(false, collBar.dropped);
    }

    // Drop a collection so that it's metadata is left over on the config server's
    // config.collections as 'dropped: true'
    st.s.getDB('sharded').foo.drop();
    assert.eq(true, csrs_config_db.collections.findOne({_id: 'sharded.foo'}).dropped);
    assert.neq(null, csrs_config_db.collections.findOne({_id: 'sharded.bar'}));
}

function test1ChecksAfterUpgrade() {
    let csrs_config_db = st.configRS.getPrimary().getDB('config');

    // Check that the left over metadata at csrs config.collections has been cleaned up.
    assert.eq(null, csrs_config_db.collections.findOne({_id: 'sharded.foo'}));

    var collBar = csrs_config_db.collections.findOne({_id: 'sharded.bar'});
    assert.eq(undefined, collBar.distributionMode);
    assert.eq(undefined, collBar.dropped);
}

// Test 2: it checks that the 'allowMigrations' field in the
// config.cache.collections entries is removed when downgrading to 4.4
function test2Setup() {
    assert.commandWorked(st.s.getDB("sharded").getCollection("test2").insert({_id: 0}));
    assert.commandWorked(st.s.getDB("sharded").getCollection("test2").insert({_id: 1}));

    assert.commandWorked(st.s.adminCommand({shardCollection: 'sharded.test2', key: {_id: 1}}));

    assert.commandWorked(
        st.s.adminCommand({moveChunk: 'sharded.test2', find: {_id: 0}, to: st.rs1.getURL()}));
    assert.commandWorked(
        st.s.adminCommand({moveChunk: 'sharded.test2', find: {_id: 1}, to: st.rs0.getURL()}));

    let updateResult = st.rs0.getPrimary().getDB("config").cache.collections.updateOne(
        {_id: 'sharded.test2'}, {$set: {allowMigrations: false}});
    assert.eq(1, updateResult.modifiedCount);

    updateResult = st.rs1.getPrimary().getDB("config").cache.collections.updateOne(
        {_id: 'sharded.test2'}, {$set: {allowMigrations: false}});
    assert.eq(1, updateResult.modifiedCount);
}

function test2ChecksAfterDowngrade() {
    assert.eq(undefined,
              st.rs0.getPrimary()
                  .getDB("config")
                  .cache.collections.findOne({_id: 'sharded.test2'})
                  .allowMigrations);
    assert.eq(undefined,
              st.rs1.getPrimary()
                  .getDB("config")
                  .cache.collections.findOne({_id: 'sharded.test2'})
                  .allowMigrations);
}

function setupInitialStateOnOldVersion() {
    assert.commandWorked(st.s.adminCommand({enableSharding: 'sharded'}));

    test1Setup();
    test2Setup();
}

function runChecksAfterUpgrade() {
    test1ChecksAfterUpgrade();
}

function runChecksAfterDowngrade() {
    test2ChecksAfterDowngrade();
}

// Start a sharded cluster on version 4.4
let old_version = '4.4';
var st = new ShardingTest({
    shards: 2,
    mongos: 1,
    other: {
        mongosOptions: {binVersion: old_version},
        configOptions: {binVersion: old_version},
        shardOptions: {binVersion: old_version},

        rsOptions: {binVersion: old_version},
        rs: true,
    }
});
st.configRS.awaitReplication();

// Setup initial conditions on 4.4
setupInitialStateOnOldVersion();

// Upgrade the entire cluster to the latest version.
jsTest.log('upgrading cluster');
st.upgradeCluster('latest');
assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV}));

// Tests after upgrade
runChecksAfterUpgrade();

// Downgrade back to 4.4
jsTest.log('downgrading cluster');
assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: old_version}));
st.upgradeCluster(old_version);

// Tests after downgrade to 4.4
runChecksAfterDowngrade();

st.stop();
})();
