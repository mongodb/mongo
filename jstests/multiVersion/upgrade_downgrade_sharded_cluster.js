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

// TODO SERVER-53104: Temporarily skipping this check because at the end of the test after
// downgrading the binaries the shards fail to refresh the catalog cache due to finding an
// 'allowMigrations' field in the persisted metadata
TestData.skipCheckingIndexesConsistentAcrossCluster = true;

load('./jstests/multiVersion/libs/multi_cluster.js');  // for upgradeCluster()
load("jstests/sharding/libs/find_chunks_util.js");

// testDroppedAndDistributionModeFields: it checks two things after upgrading from versions
// prior 4.9:
// - dropped collections are not present in csrs config.collections
// - Entries on config.collections doesn't have the 'distributionMode' and the 'dropped' fields
//
// This test must be removed once 5.0 is defined as the lastLTS (SERVER-52630)
function testDroppedAndDistributionModeFieldsSetup() {
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

function testDroppedAndDistributionModeFieldsChecksAfterUpgrade() {
    let csrs_config_db = st.configRS.getPrimary().getDB('config');

    // Check that the left over metadata at csrs config.collections has been cleaned up.
    assert.eq(null, csrs_config_db.collections.findOne({_id: 'sharded.foo'}));

    var collBar = csrs_config_db.collections.findOne({_id: 'sharded.bar'});
    assert.eq(undefined, collBar.distributionMode);
    assert.eq(undefined, collBar.dropped);
}

// testAllowedMigrationsField: it checks that the 'allowMigrations' field in the
// config.cache.collections entries is removed when downgrading to prior 4.9
//
// This test must be removed once 5.0 is defined as the lastLTS (SERVER-52632)
function testAllowedMigrationsFieldSetup() {
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

function testAllowedMigrationsFieldChecksAfterFCVDowngrade() {
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

// testTimestampField: Check that on FCV upgrade to 5.0, a 'timestamp' is created for the existing
// collections in config.databases, config.cache.databases, config.collections and
// config.cache.collections. On downgrade, check that the 'timestamp' field is removed.
//
// This test must be removed once 5.0 is defined as the lastLTS.
function testTimestampFieldSetup() {
    assert.commandWorked(
        st.s.adminCommand({shardCollection: 'sharded.test3_created_before_upgrade', key: {x: 1}}));
    assert.commandWorked(
        st.s.adminCommand({split: 'sharded.test3_created_before_upgrade', middle: {x: 10}}));
    assert.commandWorked(
        st.s.adminCommand({split: 'sharded.test3_created_before_upgrade', middle: {x: -10}}));
}

function testTimestampFieldChecksAfterUpgrade() {
    let csrs_config_db = st.configRS.getPrimary().getDB('config');

    // Check that 'timestamp' has been created in configsvr config.databases
    let dbTimestampInConfigSvr =
        csrs_config_db.databases.findOne({_id: 'sharded'}).version.timestamp;
    assert.neq(null, dbTimestampInConfigSvr);

    // Check that 'timestamp' propagates to the shardsvr config.cache.databases
    let primaryShard = st.getPrimaryShard('sharded');
    primaryShard.adminCommand({
        _flushDatabaseCacheUpdates: 'sharded',
        syncFromConfig: true
    });  // TODO: After SERVER-53104 it won't be necessary to issue this flush command from the
         // test.
    assert.eq(
        dbTimestampInConfigSvr,
        primaryShard.getDB('config').cache.databases.findOne({_id: 'sharded'}).version.timestamp);

    // Check that 'timestamp' has been created in configsvr config.collections
    let collTimestampInConfigSvr =
        csrs_config_db.collections.findOne({_id: 'sharded.test3_created_before_upgrade'}).timestamp;
    assert.neq(null, collTimestampInConfigSvr);

    // TODO: After SERVER-52587, check that the timestamp in the shardsvr config.cache.collection
    // exists and matches collTimestampInConfigSvr

    // Check that 'timestamp' has been created in config.chunks
    var cursor = findChunksUtil.findChunksByNs(st.config, 'sharded.test3_created_before_upgrade');
    assert(cursor.hasNext());
    do {
        assert.eq(collTimestampInConfigSvr, cursor.next().lastmodTimestamp);
    } while (cursor.hasNext());
}

function testTimestampFieldSetupBeforeDowngrade() {
    assert.commandWorked(
        st.s.adminCommand({shardCollection: 'sharded.test3_created_after_upgrade', key: {x: 1}}));
}

function testTimestampFieldChecksAfterFCVDowngrade() {
    let csrs_config_db = st.configRS.getPrimary().getDB('config');
    let primaryShard = st.getPrimaryShard('sharded');

    // Check that the 'timestamp' has been removed from config.databases
    assert.eq(null, csrs_config_db.databases.findOne({_id: 'sharded'}).version.timestamp);

    // Check that the 'timestamp' has been removed from config.cache.databases.
    assert.eq(
        null,
        primaryShard.getDB('config').cache.databases.findOne({_id: 'sharded'}).version.timestamp);

    // Check that the 'timestamp' has been removed from config.collections.
    let collAfterUpgrade =
        csrs_config_db.collections.findOne({_id: 'sharded.test3_created_before_upgrade'});
    assert.eq(null, collAfterUpgrade.timestamp);

    // Check that the 'timestamp' has been removed from config.cache.collections.
    let timestampInShard =
        primaryShard.getDB('config')
            .cache.collections.findOne({_id: 'sharded.test3_created_before_upgrade'})
            .timestamp;
    assert.eq(null, timestampInShard);

    // Check that the 'timestamp' has been removed from config.chunks
    var cursor = findChunksUtil.findChunksByNs(st.config, 'sharded.test3_created_before_upgrade');
    assert(cursor.hasNext());
    do {
        assert.eq(null, cursor.next().lastmodTimestamp);
    } while (cursor.hasNext());

    // TODO: After SERVER-52587, this is no longer needed as we can just check with
    // test3_created_before_upgrade.
    timestampInShard = primaryShard.getDB('config')
                           .cache.collections.findOne({_id: 'sharded.test3_created_after_upgrade'})
                           .timestamp;
    assert.eq(null, timestampInShard);
}

// testChunkCollectionUuidField: ensures all config.chunks entries include a collection UUID after
// upgrading from versions prior 4.9; and that it is deleted on downgrade.
//
// This test must be removed once 5.0 is defined as the lastLTS (SERVER-52630)
function testChunkCollectionUuidFieldSetup() {
    const ns = "sharded.test_chunk_uuid";

    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 1}}));
    assert.commandWorked(st.s.adminCommand({split: ns, middle: {x: 10}}));
    assert.commandWorked(st.s.adminCommand({split: ns, middle: {x: -10}}));
}

function testChunkCollectionUuidFieldChecksAfterUpgrade() {
    const ns = "sharded.test_chunk_uuid";

    var collUUID = st.config.collections.findOne({_id: ns}).uuid;
    var cursor = findChunksUtil.findChunksByNs(st.config, ns);
    assert(cursor.hasNext());
    do {
        assert.eq(collUUID, cursor.next().uuid);
    } while (cursor.hasNext());

    // Check no chunk with ns is left after upgrade
    assert.eq(0, st.config.chunks.count({ns: {$exists: true}}));
}

function testChunkCollectionUuidFieldChecksAfterFCVDowngrade() {
    const ns = "sharded.test_chunk_uuid";

    var cursor = findChunksUtil.findChunksByNs(st.config, ns);
    assert(cursor.hasNext());
    do {
        assert.eq(undefined, cursor.next().uuid);
    } while (cursor.hasNext());
}

function setupInitialStateOnOldVersion() {
    assert.commandWorked(st.s.adminCommand({enableSharding: 'sharded'}));

    testDroppedAndDistributionModeFieldsSetup();
    testAllowedMigrationsFieldSetup();
    testTimestampFieldSetup();
    testChunkCollectionUuidFieldSetup();
}

function runChecksAfterUpgrade() {
    const isFeatureFlagEnabled =
        assert
            .commandWorked(st.configRS.getPrimary().adminCommand(
                {getParameter: 1, featureFlagShardingFullDDLSupportTimestampedVersion: 1}))
            .featureFlagShardingFullDDLSupportTimestampedVersion.value;

    testDroppedAndDistributionModeFieldsChecksAfterUpgrade();

    if (isFeatureFlagEnabled) {
        testTimestampFieldChecksAfterUpgrade();
        testChunkCollectionUuidFieldChecksAfterUpgrade();
    } else {
        jsTest.log(
            'Skipping tests that require featureFlagShardingFullDDLSupportTimestampedVersion feature to be enabled');
    }
}

function setupStateBeforeDowngrade() {
    testTimestampFieldSetupBeforeDowngrade();
}

function runChecksAfterFCVDowngrade() {
    const isFeatureFlagEnabled =
        assert
            .commandWorked(st.configRS.getPrimary().adminCommand(
                {getParameter: 1, featureFlagShardingFullDDLSupportTimestampedVersion: 1}))
            .featureFlagShardingFullDDLSupportTimestampedVersion.value;

    if (isFeatureFlagEnabled) {
        testAllowedMigrationsFieldChecksAfterFCVDowngrade();
        testTimestampFieldChecksAfterFCVDowngrade();
        testChunkCollectionUuidFieldChecksAfterFCVDowngrade();
    } else {
        jsTest.log(
            'Skipping tests that require featureFlagShardingFullDDLSupportTimestampedVersion feature to be enabled');
    }
}

function runChecksAfterBinDowngrade() {
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

    const isFeatureFlagEnabled =
        assert
            .commandWorked(st.configRS.getPrimary().adminCommand(
                {getParameter: 1, featureFlagShardingFullDDLSupportTimestampedVersion: 1}))
            .featureFlagShardingFullDDLSupportTimestampedVersion.value;

    if (isFeatureFlagEnabled) {
        // TODO SERVER-53104: do not skip this test once this ticket is completed
        jsTest.log(
            'Skipping test since it is not compatible with featureFlagShardingFullDDLSupportTimestampedVersion yet');
        st.stop();
        return;
    }

    assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV}));

    // Tests after upgrade
    runChecksAfterUpgrade();

    // Setup state before downgrade
    setupStateBeforeDowngrade();

    // Downgrade FCV back to oldVersion
    jsTest.log('downgrading cluster FCV');
    assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: oldVersion}));

    // Tests after FCV downgrade to oldVersion
    runChecksAfterFCVDowngrade();

    // Downgrade binaries back to oldVersion
    jsTest.log('downgrading cluster binaries');
    st.upgradeCluster(oldVersion);

    // Tests after binaries downgrade to oldVersion
    runChecksAfterBinDowngrade();

    st.stop();
}
})();
