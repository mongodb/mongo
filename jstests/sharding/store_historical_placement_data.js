
(function() {
"use strict";
load("jstests/libs/feature_flag_util.js");

const st = new ShardingTest({shards: 2});
const configDB = st.s.getDB('config');

function getAndValidateLatestPlacementInfoForDB(dbName) {
    const placementQueryResults =
        configDB.placementHistory.find({nss: dbName}).sort({timestamp: -1}).limit(1).toArray();
    assert.eq(placementQueryResults.length, 1);
    const dbPlacementDetails = placementQueryResults[0];

    // Verify that the placementHistory document matches the related content stored in
    // config.databases.
    const configDBsQueryResults = configDB.databases.find({_id: dbPlacementDetails.nss}).toArray();
    assert.eq(1, configDBsQueryResults.length);
    const databaseDetails = configDBsQueryResults[0];

    assert(timestampCmp(databaseDetails.version.timestamp, dbPlacementDetails.timestamp) === 0);
    assert.eq(1, dbPlacementDetails.shards.length);
    assert.eq(databaseDetails.primary, dbPlacementDetails.shards[0]);
    assert.eq(undefined, dbPlacementDetails.uuid);
    return dbPlacementDetails;
}

function getAndValidateLatestPlacementInfoForCollection(fullCollName) {
    const placementQueryResults = configDB.placementHistory.find({nss: fullCollName})
                                      .sort({timestamp: -1})
                                      .limit(1)
                                      .toArray();
    assert.eq(placementQueryResults.length, 1);
    const placementDetails = placementQueryResults[0];

    // Verify that the placementHistory document matches the related content stored in
    // config.collections.
    const configCollsQueryResults =
        configDB.collections.find({_id: placementDetails.nss}).toArray();
    assert.eq(configCollsQueryResults.length, 1);
    const collectionEntry = configCollsQueryResults[0];

    assert.eq(collectionEntry.uuid, placementDetails.uuid);
    assert(timestampCmp(collectionEntry.timestamp, placementDetails.timestamp) === 0);
    return placementDetails;
}

function testEnableSharding(dbName, primaryShardName) {
    assert.commandWorked(
        st.s.adminCommand({enableSharding: dbName, primaryShard: primaryShardName}));
    getAndValidateLatestPlacementInfoForDB(dbName);
}

function testShardCollection(dbName, collName) {
    let nss = dbName + '.' + collName;

    // Shard the collection. Ensure enough chunks to cover all shards.
    assert.commandWorked(
        st.s.adminCommand({shardCollection: nss, key: {_id: "hashed"}, numInitialChunks: 20}));
    // Verify that there is consistent placement info on the shared collection and its parent DB.
    const dbPlacementInfo = getAndValidateLatestPlacementInfoForDB(dbName);
    const collPlacementInfo = getAndValidateLatestPlacementInfoForCollection(nss);
    assert(timestampCmp(dbPlacementInfo.timestamp, collPlacementInfo.timestamp) < 0);

    // Verify that the placementHistory document matches the related content stored in
    // config.shards.
    const entriesInConfigShards = configDB.shards.find({}, {_id: 1}).toArray().map((s) => s._id);
    assert.sameMembers(entriesInConfigShards, collPlacementInfo.shards);
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

st.stop();
}());
