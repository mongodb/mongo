/**
 * The goal of this test is to verify that some metadata is properly updated when
 *upgrading/downgrading a sharded cluster. More specifically:
 *
 *	1. We create a sharded cluster running and old binary version (lastLTSFCV or lastContinuousFCV)
 *	2. We run some operations that involve the creation of some metadata
 *	3. We upgrade the binaries of the sharded cluster to the latest version + set FCV to latestFCV
 *	4. We verify that the metadata has been properly upgraded
 *	5. We set FCV to old bin version + downgrade the binaries of the sharded cluster to that version
 *	6. We verify that the metadata has been properly downgraded
 */
(function() {
"use strict";

load('./jstests/multiVersion/libs/multi_cluster.js');  // for upgradeCluster()

// Test 1: it checks two things after upgrading from versions prior 4.9:
// - dropped collections are not present in csrs config.collections
// - Entries on config.collections doesn't have the 'distributionMode' and the 'dropped' fields
//
// This test must be removed once 5.0 is defined as the lastLTS (SERVER-52630)
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
// config.cache.collections entries is removed when downgrading to prior 4.9
//
// This test must be removed once 5.0 is defined as the lastLTS (SERVER-52632)
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

for (let oldVersion of [lastLTSFCV, lastContinuousFCV]) {
    var st = new ShardingTest({
        shards: 2,
        mongos: 1,
        other: {
            mongosOptions: {binVersion: oldVersion},
            configOptions: {binVersion: oldVersion},
            shardOptions: {binVersion: oldVersion},

            rsOptions: {binVersion: oldVersion},
            rs: true,
        }
    });

    st.configRS.awaitReplication();

    // Setup initial conditions
    setupInitialStateOnOldVersion();

    // Upgrade the entire cluster to the latest version.
    jsTest.log('upgrading cluster');
    st.upgradeCluster(latestFCV);
    assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV}));

    // Tests after upgrade
    runChecksAfterUpgrade();

    // Downgrade back to oldVersion
    jsTest.log('downgrading cluster');
    assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: oldVersion}));
    st.upgradeCluster(oldVersion);

    // Tests after downgrade to oldVersion
    runChecksAfterDowngrade();

    st.stop();
}
})();
