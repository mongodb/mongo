/**
 * This test checks several upgrade/downgrade routines related to sharding, namely:
 *  - The entries in the config server's config.collections that were marked as 'dropped: true' are
 * deleted. (SERVER-52630: Remove once 5.0 becomes the LastLTS)
 */
(function() {
"use strict";

load('./jstests/multiVersion/libs/multi_cluster.js');  // for upgradeCluster()

function setupInitialStateOnOldVersion() {
    let csrs_config_db = st.configRS.getPrimary().getDB('config');

    // Setup sharded collections
    assert.commandWorked(st.s.adminCommand({enableSharding: 'sharded'}));
    assert.commandWorked(st.s.adminCommand({shardCollection: 'sharded.foo', key: {x: 1}}));
    assert.commandWorked(st.s.adminCommand({shardCollection: 'sharded.bar', key: {x: 1}}));

    assert.neq(null, csrs_config_db.collections.findOne({_id: 'sharded.foo'}));
    assert.neq(null, csrs_config_db.collections.findOne({_id: 'sharded.bar'}));

    // Drop a collection so that it's metadata is left over on the config server's
    // config.collections as 'dropped: true'
    st.s.getDB('sharded').foo.drop();
    assert.eq(true, csrs_config_db.collections.findOne({_id: 'sharded.foo'}).dropped);
    assert.neq(null, csrs_config_db.collections.findOne({_id: 'sharded.bar'}));
}

function runChecksAfterUpgrade() {
    let csrs_config_db = st.configRS.getPrimary().getDB('config');
    // Check that the left over metadata at csrs config.collections has been cleaned up.
    assert.eq(null, csrs_config_db.collections.findOne({_id: 'sharded.foo'}));
    assert.neq(null, csrs_config_db.collections.findOne({_id: 'sharded.bar'}));
}

function runChecksAfterDowngrade() {
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
