/**
 * The goal of this test is to verify that some metadata is properly updated when
 *upgrading/downgrading a sharded cluster. More specifically:
 *
 *	1. We create a sharded cluster running and old binary version (lastLTSFCV)
 *	2. We run some operations that involve the creation of some metadata
 *	3. We upgrade the binaries of the sharded cluster to the latest version + set FCV to latestFCV
 *	4. We verify that the metadata has been properly upgraded
 *	5. We set FCV to old bin version + downgrade the binaries of the sharded cluster to that version
 *	6. We verify that the metadata has been properly downgraded
 */
(function() {
"use strict";

load('./jstests/multiVersion/libs/multi_cluster.js');  // for upgradeCluster()
load("jstests/sharding/libs/find_chunks_util.js");

// testDroppedAndDistributionModeFields: it checks two things after upgrading from versions
// prior 4.9:
// - dropped collections are not present in csrs config.collections
// - Entries on config.collections doesn't have the 'distributionMode' and the 'dropped' fields
//
// This test must be removed once 5.0 is defined as the lastLTS (SERVER-52630)
function testDroppedAndDistributionModeFieldsSetup(oldVersion) {
    let configDB = st.s.getDB('config');
    // Setup sharded collections
    assert.commandWorked(st.s.adminCommand({shardCollection: 'sharded.foo', key: {x: 1}}));
    assert.commandWorked(st.s.adminCommand({shardCollection: 'sharded.bar', key: {x: 1}}));

    if (oldVersion == lastLTSFCV) {
        var collFoo = configDB.collections.findOne({_id: 'sharded.foo'});
        assert.eq('sharded', collFoo.distributionMode);
        assert.eq(false, collFoo.dropped);

        var collBar = configDB.collections.findOne({_id: 'sharded.bar'});
        assert.eq('sharded', collBar.distributionMode);
        assert.eq(false, collBar.dropped);
    }

    // Drop a collection so that in a 4.4 binary, its metadata is left over on the config server's
    // config.collections as 'dropped: true'
    st.s.getDB('sharded').foo.drop();
    if (oldVersion == lastLTSFCV) {
        assert.eq(true, configDB.collections.findOne({_id: 'sharded.foo'}).dropped);
        assert.neq(null, configDB.collections.findOne({_id: 'sharded.bar'}));
    }
}

function testDroppedAndDistributionModeFieldsChecksAfterUpgrade() {
    let configDB = st.s.getDB('config');

    // Check that the left over metadata at csrs config.collections has been cleaned up.
    assert.eq(null, configDB.collections.findOne({_id: 'sharded.foo'}));

    var collBar = configDB.collections.findOne({_id: 'sharded.bar'});
    assert.eq(undefined, collBar.distributionMode);
    assert.eq(undefined, collBar.dropped);
}

// testAllowedMigrationsField: it checks that the 'allowMigrations' field in the
// config.cache.collections entries is removed when downgrading to prior 4.9
//
// This test must be removed once 5.0 is defined as the lastLTS (SERVER-52632)

function checkAllowMigrationsOnConfigAndShardMetadata(expectedResult) {
    const ns = "sharded.test2";
    assert.eq(
        expectedResult,
        st.configRS.getPrimary().getDB("config").collections.findOne({_id: ns}).allowMigrations);
    assert.eq(
        expectedResult,
        st.rs0.getPrimary().getDB("config").cache.collections.findOne({_id: ns}).allowMigrations);
    assert.eq(
        expectedResult,
        st.rs1.getPrimary().getDB("config").cache.collections.findOne({_id: ns}).allowMigrations);
}

function testAllowedMigrationsFieldSetup() {
    const ns = "sharded.test2";
    assert.commandWorked(st.s.getDB("sharded").getCollection("test2").insert({_id: 0}));
    assert.commandWorked(st.s.getDB("sharded").getCollection("test2").insert({_id: 1}));

    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));

    assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {_id: 0}, to: st.rs1.getURL()}));
    assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {_id: 1}, to: st.rs0.getURL()}));

    // If allowMigrations is true, it means that the allowMigration field is not defined in
    // config.collections neither on config.cache.collections
    assert.commandWorked(st.configRS.getPrimary().adminCommand(
        {_configsvrSetAllowMigrations: ns, allowMigrations: true, writeConcern: {w: "majority"}}));

    assert.commandWorked(
        st.shard0.adminCommand({_flushRoutingTableCacheUpdates: ns, syncFromConfig: true}));
    assert.commandWorked(
        st.shard1.adminCommand({_flushRoutingTableCacheUpdates: ns, syncFromConfig: true}));

    checkAllowMigrationsOnConfigAndShardMetadata(undefined);
}

function testAllowedMigrationsFieldChecksAfterFCVDowngrade() {
    checkAllowMigrationsOnConfigAndShardMetadata(undefined);
}

// testTimestampField: Check that on FCV upgrade to 5.0, a 'timestamp' is created for the existing
// collections in config.databases, config.cache.databases, config.collections and
// config.cache.collections. On downgrade, check that the 'timestamp' field is removed.
//
// This test must be removed once 5.0 is defined as the lastLTS.
function testTimestampFieldSetup() {
    assert.commandWorked(st.s.adminCommand({shardCollection: 'sharded.test3', key: {x: 1}}));
    assert.commandWorked(st.s.adminCommand({split: 'sharded.test3', middle: {x: 10}}));
    assert.commandWorked(st.s.adminCommand({split: 'sharded.test3', middle: {x: -10}}));
}

function testTimestampFieldChecksAfterUpgrade() {
    let configDB = st.s.getDB('config');

    // Check that 'timestamp' has been created in configsvr config.databases
    let dbTimestampInConfigSvr = configDB.databases.findOne({_id: 'sharded'}).version.timestamp;
    assert.neq(null, dbTimestampInConfigSvr);

    let primaryShard = st.getPrimaryShard('sharded');

    // Check that 'timestamp' propagates to the shardsvr config.cache.databases
    assert.eq(
        dbTimestampInConfigSvr,
        primaryShard.getDB('config').cache.databases.findOne({_id: 'sharded'}).version.timestamp);

    // Check that 'timestamp' has been created in configsvr config.collections
    let collTimestampInConfigSvr = configDB.collections.findOne({_id: 'sharded.test3'}).timestamp;
    assert.neq(null, collTimestampInConfigSvr);

    // Check that 'timestamp' has been propagated to config.cache.collections
    assert.eq(
        collTimestampInConfigSvr,
        primaryShard.getDB('config').cache.collections.findOne({_id: 'sharded.test3'}).timestamp);

    // Check that config.chunks doesn't have epochs neither timestamps
    var cursor = findChunksUtil.findChunksByNs(st.config, 'sharded.test3');
    assert(cursor.hasNext());
    do {
        var chunk = cursor.next();
        assert.eq(null, chunk.lastmodEpoch);
        assert.eq(null, chunk.lastmodTimestamp);
    } while (cursor.hasNext());
}

function testTimestampFieldChecksAfterFCVDowngrade() {
    let configDB = st.s.getDB('config');
    let primaryShard = st.getPrimaryShard('sharded');

    // Check that the 'timestamp' has been removed from config.databases
    assert.eq(null, configDB.databases.findOne({_id: 'sharded'}).version.timestamp);

    // Check that the 'timestamp' has been removed from config.cache.databases.
    assert.eq(
        null,
        primaryShard.getDB('config').cache.databases.findOne({_id: 'sharded'}).version.timestamp);

    // Check that the 'timestamp' has been removed from config.collections.
    let collAfterUpgrade = configDB.collections.findOne({_id: 'sharded.test3'});
    assert.eq(null, collAfterUpgrade.timestamp);

    // Check that the 'timestamp' has been removed from config.cache.collections.
    let timestampInShard =
        primaryShard.getDB('config').cache.collections.findOne({_id: 'sharded.test3'}).timestamp;
    assert.eq(null, timestampInShard);

    // Check that the 'timestamp' has been removed from config.chunks
    var cursor = findChunksUtil.findChunksByNs(st.config, 'sharded.test3');
    assert(cursor.hasNext());
    do {
        var chunk = cursor.next();
        assert.eq(collAfterUpgrade.lastmodEpoch, chunk.lastmodEpoch);
        assert.eq(null, chunk.lastmodTimestamp);
    } while (cursor.hasNext());
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

function setupInitialStateOnOldVersion(oldVersion) {
    assert.commandWorked(st.s.adminCommand({enableSharding: 'sharded'}));

    testDroppedAndDistributionModeFieldsSetup(oldVersion);
    testTimestampFieldSetup();
    testChunkCollectionUuidFieldSetup();
}

function checkConfigAndShardsFCV(expectedFCV) {
    var configFCV = st.configRS.getPrimary()
                        .adminCommand({getParameter: 1, featureCompatibilityVersion: 1})
                        .featureCompatibilityVersion.version;
    assert.eq(expectedFCV, configFCV);

    var shard0FCV = st.rs0.getPrimary()
                        .adminCommand({getParameter: 1, featureCompatibilityVersion: 1})
                        .featureCompatibilityVersion.version;
    assert.eq(expectedFCV, shard0FCV);

    var shard1FCV = st.rs1.getPrimary()
                        .adminCommand({getParameter: 1, featureCompatibilityVersion: 1})
                        .featureCompatibilityVersion.version;
    assert.eq(expectedFCV, shard1FCV);
}

function runChecksAfterUpgrade() {
    checkConfigAndShardsFCV(latestFCV);

    testDroppedAndDistributionModeFieldsChecksAfterUpgrade();
    testTimestampFieldChecksAfterUpgrade();
    testChunkCollectionUuidFieldChecksAfterUpgrade();
}

function setupStateBeforeDowngrade() {
    testAllowedMigrationsFieldSetup();
}

function runChecksAfterFCVDowngrade(oldVersion) {
    checkConfigAndShardsFCV(oldVersion);

    testAllowedMigrationsFieldChecksAfterFCVDowngrade();
    testTimestampFieldChecksAfterFCVDowngrade();
    testChunkCollectionUuidFieldChecksAfterFCVDowngrade();
}

function runChecksAfterBinDowngrade() {
}

{
    let oldVersion = lastLTSFCV;
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

    jsTest.log('oldVersion: ' + oldVersion);

    st.configRS.awaitReplication();

    // Setup initial conditions
    setupInitialStateOnOldVersion(oldVersion);

    // Upgrade the entire cluster to the latest version.
    jsTest.log('upgrading cluster binaries');
    st.upgradeCluster(latestFCV);

    jsTest.log('upgrading cluster FCV');
    assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV}));

    // Tests after upgrade
    runChecksAfterUpgrade();

    // Setup state before downgrade
    setupStateBeforeDowngrade();

    // Downgrade FCV back to oldVersion
    jsTest.log('downgrading cluster FCV');
    assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: oldVersion}));

    // Tests after FCV downgrade to oldVersion
    runChecksAfterFCVDowngrade(oldVersion);

    // Downgrade binaries back to oldVersion
    jsTest.log('downgrading cluster binaries');
    st.upgradeCluster(oldVersion);

    // Tests after binaries downgrade to oldVersion
    runChecksAfterBinDowngrade();

    st.stop();
}
})();
