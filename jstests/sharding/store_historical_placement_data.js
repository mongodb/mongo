
(function() {
"use strict";
load("jstests/libs/feature_flag_util.js");

const st = new ShardingTest({shards: 2});
const configDB = st.s.getDB('config');

function getInfoFromConfigDatabases(dbName) {
    const configDBsQueryResults = configDB.databases.find({_id: dbName}).toArray();
    if (configDBsQueryResults.length === 0) {
        return null;
    }
    assert.eq(1, configDBsQueryResults.length);
    return configDBsQueryResults[0];
}

function getInfoFromConfigCollections(fullCollName) {
    const configCollsQueryResults = configDB.collections.find({_id: fullCollName}).toArray();
    if (configCollsQueryResults.length === 0) {
        return null;
    }
    assert.eq(configCollsQueryResults.length, 1);
    return configCollsQueryResults[0];
}

function getLatestPlacementInfoFor(namespace) {
    const placementQueryResults =
        configDB.placementHistory.find({nss: namespace}).sort({timestamp: -1}).limit(1).toArray();
    assert.eq(placementQueryResults.length, 1);
    return placementQueryResults[0];
}

function getValidatedPlacementInfoForDB(dbName, isInitialPlacement = true) {
    const configDBInfo = getInfoFromConfigDatabases(dbName);
    const dbPlacementInfo = getLatestPlacementInfoFor(dbName);
    assert.neq(null, configDBInfo);
    assert.neq(null, dbPlacementInfo);

    // Verify that the placementHistory document matches the related content stored in
    // config.databases.
    assert.sameMembers([configDBInfo.primary], dbPlacementInfo.shards);

    if (isInitialPlacement) {
        assert(timestampCmp(configDBInfo.version.timestamp, dbPlacementInfo.timestamp) === 0);
    } else {
        // after a movePrimary, the timestamp of the placementHistory document should be greater
        // since the timestamp associated to the config.databases document does not change (only
        // lastMod is updated).
        assert(timestampCmp(configDBInfo.version.timestamp, dbPlacementInfo.timestamp) < 0);
    }

    // No UUID field for DB namespaces
    assert.eq(undefined, dbPlacementInfo.uuid);
    return dbPlacementInfo;
}

function getValidatedPlacementInfoForCollection(
    dbName, collName, expectedShardList, calledOnCreation = false) {
    const fullName = dbName + '.' + collName;
    const configCollInfo = getInfoFromConfigCollections(fullName);
    assert.neq(null, configCollInfo);

    // Verify that there is consistent placement info on the sharded collection and its parent DB.
    const collPlacementInfo = getLatestPlacementInfoFor(fullName);
    assert.neq(null, collPlacementInfo);
    const dbPlacementInfo = getValidatedPlacementInfoForDB(dbName);
    assert(timestampCmp(dbPlacementInfo.timestamp, collPlacementInfo.timestamp) < 0);

    assert.eq(configCollInfo.uuid, collPlacementInfo.uuid);
    if (calledOnCreation) {
        assert(timestampCmp(configCollInfo.timestamp, collPlacementInfo.timestamp) === 0);
    } else {
        assert(timestampCmp(configCollInfo.timestamp, collPlacementInfo.timestamp) <= 0);
    }

    assert.sameMembers(expectedShardList, collPlacementInfo.shards);
    return collPlacementInfo;
}

function testEnableSharding(dbName, primaryShardName) {
    assert.commandWorked(
        st.s.adminCommand({enableSharding: dbName, primaryShard: primaryShardName}));
    getValidatedPlacementInfoForDB(dbName);
}

function testShardCollection(dbName, collName) {
    const nss = dbName + '.' + collName;

    // Shard the collection. Ensure enough chunks to cover all shards.
    assert.commandWorked(
        st.s.adminCommand({shardCollection: nss, key: {_id: "hashed"}, numInitialChunks: 20}));

    // Verify that a consistent document has been added to config.placementHistory and that its list
    // of shards matches the current content of config.shards
    const entriesInConfigShards = configDB.shards.find({}, {_id: 1}).toArray().map((s) => s._id);
    getValidatedPlacementInfoForCollection(dbName, collName, entriesInConfigShards, true);
}

function testMovePrimary(dbName, fromPrimaryShardName, toPrimaryShardName) {
    // Create the database
    testEnableSharding(dbName, fromPrimaryShardName);

    // Move the primary shard
    assert.commandWorked(st.s.adminCommand({movePrimary: dbName, to: toPrimaryShardName}));

    // Verify that the new primary shard is the one specified in the command.
    const newDbInfo = getValidatedPlacementInfoForDB(dbName, false /* isInitialPlacement */);
    assert.sameMembers(newDbInfo.shards, [toPrimaryShardName]);
}

// TODO SERVER-69106 remove the logic to skip the test execution
const historicalPlacementDataFeatureFlag = FeatureFlagUtil.isEnabled(
    st.configRS.getPrimary().getDB('admin'), "HistoricalPlacementShardingCatalog");
if (!historicalPlacementDataFeatureFlag) {
    jsTestLog("Skipping as featureFlagHistoricalPlacementShardingCatalog is disabled");
    st.stop();
    return;
}

jsTest.log('Testing placement entries added by explicit DB creation');
testEnableSharding('explicitlyCreatedDB', st.shard0.shardName);

jsTest.log(
    'Testing placement entries added by shardCollection() over an existing sharding-enabled DB');
testShardCollection('explicitlyCreatedDB', 'coll1');

jsTest.log('Testing placement entries added by shardCollection() over a non-existing db (& coll)');
testShardCollection('implicitlyCreatedDB', 'coll1');

jsTest.log(
    'Testing placement entries added by movePrimary() over a new sharding-enabled DB with no data');
testMovePrimary('movePrimaryDB', st.shard0.shardName, st.shard1.shardName);

st.stop();
}());
