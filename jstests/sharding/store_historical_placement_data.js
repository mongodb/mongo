
(function() {
"use strict";
load("jstests/libs/feature_flag_util.js");

const st = new ShardingTest({shards: 3, chunkSize: 1});
const configDB = st.s.getDB('config');
const shard0 = st.shard0.shardName;
const shard1 = st.shard1.shardName;
const shard2 = st.shard2.shardName;

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

function getLatestPlacementEntriesFor(namespace, numEntries) {
    return configDB.placementHistory.find({nss: namespace})
        .sort({timestamp: -1})
        .limit(numEntries)
        .toArray();
}

function getLatestPlacementInfoFor(namespace) {
    const placementQueryResults = getLatestPlacementEntriesFor(namespace, 1);
    if (placementQueryResults.length === 0) {
        return null;
    }

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
    dbName, collName, expectedShardList, isInitialPlacement = false) {
    const fullName = dbName + '.' + collName;
    const configCollInfo = getInfoFromConfigCollections(fullName);
    assert.neq(null, configCollInfo);

    // Verify that there is consistent placement info on the sharded collection and its parent DB.
    const collPlacementInfo = getLatestPlacementInfoFor(fullName);
    assert.neq(null, collPlacementInfo);
    const dbPlacementInfo = getValidatedPlacementInfoForDB(dbName);
    assert(timestampCmp(dbPlacementInfo.timestamp, collPlacementInfo.timestamp) < 0);

    assert.eq(configCollInfo.uuid, collPlacementInfo.uuid);
    if (isInitialPlacement) {
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

function testMoveChunk(dbName, collName) {
    // Setup - All the data are contained by a single chunk on the primary shard
    const nss = dbName + '.' + collName;
    testEnableSharding(dbName, shard0);
    assert.commandWorked(st.s.adminCommand({shardCollection: nss, key: {x: 1}}));
    const collPlacementInfoAtCreationTime =
        getValidatedPlacementInfoForCollection(dbName, collName, [shard0], true);
    const collUUID = collPlacementInfoAtCreationTime.uuid;
    assert.eq(1, configDB.chunks.count({uuid: collUUID}));

    // Create two chunks, then move 1 to shard1 -> the recipient should be present in a new
    // placement entry
    st.s.adminCommand({split: nss, middle: {x: 0}});
    assert.commandWorked(st.s.adminCommand({moveChunk: nss, find: {x: -1}, to: shard1}));
    let placementAfterMigration =
        getValidatedPlacementInfoForCollection(dbName, collName, [shard0, shard1]);
    let migratedChunk = configDB.chunks.findOne({uuid: collUUID, min: {x: MinKey}});
    assert(timestampCmp(placementAfterMigration.timestamp, migratedChunk.history[0].validAfter) ===
           0);
    assert(timestampCmp(placementAfterMigration.timestamp, migratedChunk.onCurrentShardSince) ===
           0);

    // Move out the last chunk from shard0 to shard2 - a new placement entry should appear, where
    // the donor has been removed and the recipient inserted
    assert.commandWorked(st.s.adminCommand({moveChunk: nss, find: {x: 1}, to: shard2}));
    placementAfterMigration =
        getValidatedPlacementInfoForCollection(dbName, collName, [shard1, shard2]);
    migratedChunk = configDB.chunks.findOne({uuid: collUUID, min: {x: 0}});
    assert(timestampCmp(placementAfterMigration.timestamp, migratedChunk.history[0].validAfter) ===
           0);
    assert(timestampCmp(placementAfterMigration.timestamp, migratedChunk.onCurrentShardSince) ===
           0);

    // Create a third chunk in shard1, then move it to shard2: since this migration does not alter
    // the subset of shards owning collection data, no new record should be inserted
    const numPlacementEntriesBeforeMigration = configDB.placementHistory.count({nss: nss});
    st.s.adminCommand({split: nss, middle: {x: 10}});
    assert.commandWorked(st.s.adminCommand({moveChunk: nss, find: {x: 10}, to: shard1}));
    const numPlacementEntriesAfterMigration = configDB.placementHistory.count({nss: nss});
    assert.eq(numPlacementEntriesBeforeMigration, numPlacementEntriesAfterMigration);
}

function testMoveRange(dbName, collName) {
    // Setup - All the data are contained by a single chunk on the primary shard
    const nss = dbName + '.' + collName;
    testEnableSharding(dbName, shard0);
    assert.commandWorked(st.s.adminCommand({shardCollection: nss, key: {x: 1}}));
    const collPlacementInfoAtCreationTime =
        getValidatedPlacementInfoForCollection(dbName, collName, [shard0], true);
    const collUUID = collPlacementInfoAtCreationTime.uuid;
    assert.eq(1, configDB.chunks.count({uuid: collUUID}));

    // Move half of the existing chunk to shard 1 -> the recipient should be added to the placement
    // data
    assert.commandWorked(
        st.s.adminCommand({moveRange: nss, min: {x: MinKey}, max: {x: 0}, toShard: shard1}));
    let placementAfterMigration =
        getValidatedPlacementInfoForCollection(dbName, collName, [shard0, shard1]);
    let migratedChunk = configDB.chunks.findOne({uuid: collUUID, min: {x: MinKey}});
    assert(timestampCmp(placementAfterMigration.timestamp, migratedChunk.history[0].validAfter) ===
           0);
    assert(timestampCmp(placementAfterMigration.timestamp, migratedChunk.onCurrentShardSince) ===
           0);

    // Move the other half to shard 1 -> shard 0 should be removed from the placement data
    assert.commandWorked(
        st.s.adminCommand({moveRange: nss, min: {x: 0}, max: {x: MaxKey}, toShard: shard1}));
    placementAfterMigration = getValidatedPlacementInfoForCollection(dbName, collName, [shard1]);
    migratedChunk = configDB.chunks.findOne({uuid: collUUID, min: {x: 0}});
    assert(timestampCmp(placementAfterMigration.timestamp, migratedChunk.history[0].validAfter) ===
           0);
    assert(timestampCmp(placementAfterMigration.timestamp, migratedChunk.onCurrentShardSince) ===
           0);
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

function testDropCollection() {
    const dbName = 'dropCollectionTestDB';
    const collName = 'shardedCollName';
    const nss = dbName + '.' + collName;
    const db = st.s.getDB(dbName);

    testShardCollection(dbName, collName);
    const initialPlacementInfo = getLatestPlacementInfoFor(nss);
    const numHistoryEntriesBeforeFirstDrop = configDB.placementHistory.count({nss: nss});

    // Drop the collection
    assert.commandWorked(db.runCommand({drop: collName}));

    // Verify that a single entry gets added with the expected content.
    const numHistoryEntriesAfterFirstDrop = configDB.placementHistory.count({nss: nss});
    assert.eq(numHistoryEntriesBeforeFirstDrop + 1, numHistoryEntriesAfterFirstDrop);
    const collPlacementInfo = getLatestPlacementInfoFor(nss);
    assert.eq(0, collPlacementInfo.shards.length);
    assert.eq(initialPlacementInfo.uuid, collPlacementInfo.uuid);
    assert(timestampCmp(initialPlacementInfo.timestamp, collPlacementInfo.timestamp) < 0);

    // Verify that no placement entry gets added if dropCollection is repeated
    assert.commandWorked(db.runCommand({drop: collName}));
    assert.eq(numHistoryEntriesAfterFirstDrop, configDB.placementHistory.count({nss: nss}));

    // Verify that no records get added in case an unsharded collection gets dropped
    const unshardedCollName = 'unshardedColl';
    assert.commandWorked(db.createCollection(unshardedCollName));

    assert.commandWorked(db.runCommand({drop: unshardedCollName}));

    assert.eq(0, configDB.placementHistory.count({nss: dbName + '.' + unshardedCollName}));
}

function testRenameCollection() {
    const dbName = 'renameCollectionTestDB';
    const db = st.s.getDB(dbName);
    const oldCollName = 'old';
    const oldNss = dbName + '.' + oldCollName;

    const targetCollName = 'target';
    const targetNss = dbName + '.' + targetCollName;

    jsTest.log(
        'Testing that placement entries are added by rename() for each sharded collection involved in the DDL');
    testShardCollection(dbName, oldCollName);
    const initialPlacementForOldColl = getLatestPlacementInfoFor(oldNss);

    assert.commandWorked(st.s.adminCommand({shardCollection: targetNss, key: {x: 1}}));
    st.s.adminCommand({split: targetNss, middle: {x: 0}});
    assert.commandWorked(st.s.adminCommand({moveChunk: targetNss, find: {x: -1}, to: shard1}));
    const initialPlacementForTargetColl = getLatestPlacementInfoFor(targetNss);

    assert.commandWorked(db[oldCollName].renameCollection(targetCollName, true /*dropTarget*/));

    // The old collection shouldn't be served by any shard anymore
    const finalPlacementForOldColl = getLatestPlacementInfoFor(oldNss);
    assert.eq(initialPlacementForOldColl.uuid, finalPlacementForOldColl.uuid);
    assert.sameMembers([], finalPlacementForOldColl.shards);

    // The target collection should have
    // - an entry for its old incarnation (no shards should serve its data) at T1
    // - an entry for its renamed incarnation (with the same properties of
    // initialPlacementForOldColl) at T2 > T1
    const [targetCollPlacementInfoWhenRenamed, targetCollPlacementInfoWhenDropped] = (function() {
        const placementEntries = getLatestPlacementEntriesFor(targetNss, 2);
        assert.eq(2, placementEntries.length);
        const placementInfoWhenDropped = placementEntries[1];
        const placementInfoWhenRenamed = placementEntries[0];
        assert(timestampCmp(placementInfoWhenDropped.timestamp,
                            placementInfoWhenRenamed.timestamp) < 0);

        return placementEntries;
    })();

    assert.eq(initialPlacementForTargetColl.uuid, targetCollPlacementInfoWhenDropped.uuid);
    assert.sameMembers([], targetCollPlacementInfoWhenDropped.shards);

    assert.eq(initialPlacementForOldColl.uuid, targetCollPlacementInfoWhenRenamed.uuid);
    assert.sameMembers(initialPlacementForOldColl.shards,
                       targetCollPlacementInfoWhenRenamed.shards);

    jsTest.log(
        'Testing that no placement entries are added by rename() for unsharded collections involved in the DDL');
    const unshardedOldCollName = 'unshardedOld';
    const unshardedTargetCollName = 'unshardedTarget';
    assert.commandWorked(db.createCollection(unshardedOldCollName));
    assert.commandWorked(db.createCollection(unshardedTargetCollName));

    assert.commandWorked(
        db[unshardedOldCollName].renameCollection(unshardedTargetCollName, true /*dropTarget*/));

    assert.eq(0, configDB.placementHistory.count({nss: dbName + '.' + unshardedOldCollName}));
    assert.eq(0, configDB.placementHistory.count({nss: dbName + '.' + unshardedTargetCollName}));
}

function testDropDatabase(dbName, primaryShardName) {
    // Create the database
    testEnableSharding(dbName, primaryShardName);
    const db = st.s.getDB(dbName);

    // Create an unsharded collection
    const unshardedCollName = 'unshardedColl';
    const unshardedCollNss = dbName + '.' + unshardedCollName;
    assert.commandWorked(db.createCollection(unshardedCollName));

    // Create a sharded collection
    const shardedCollName = 'shardedColl';
    const shardedCollNss = dbName + '.' + shardedCollName;
    testShardCollection(dbName, shardedCollName);
    const initialShardedCollPlacementInfo = getLatestPlacementInfoFor(shardedCollNss);

    // Drop the database
    assert.commandWorked(db.dropDatabase());

    // Verify that a new entry with an empty set of shards has been inserted for both dbName and
    // shardedCollName...
    const dbPlacementInfo = getLatestPlacementInfoFor(dbName);
    assert.neq(null, dbPlacementInfo);
    assert.eq(0, dbPlacementInfo.shards.length);
    assert.eq(undefined, dbPlacementInfo.uuid);

    const finalShardedCollPlacementInfo = getLatestPlacementInfoFor(shardedCollNss);
    assert.neq(null, finalShardedCollPlacementInfo);
    assert.eq(0, finalShardedCollPlacementInfo.shards);
    assert.eq(initialShardedCollPlacementInfo.uuid, finalShardedCollPlacementInfo.uuid);
    assert(timestampCmp(initialShardedCollPlacementInfo.timestamp,
                        finalShardedCollPlacementInfo.timestamp) < 0);

    // ...And that unshardedCollName stays untracked.
    assert.eq(null, getLatestPlacementInfoFor(unshardedCollNss));
}

function testReshardCollection() {
    const dbName = 'reshardCollectionTestDB';
    const collName = 'shardedCollName';
    const nss = dbName + '.' + collName;
    let db = st.s.getDB(dbName);

    // Start with a collection with a single chunk on shard0.
    assert.commandWorked(st.s.adminCommand({enableSharding: dbName, primaryShard: shard0}));
    assert.commandWorked(st.s.adminCommand({shardCollection: nss, key: {oldShardKey: 1}}));
    const initialNumPlacementEntries = configDB.placementHistory.count({});
    const initialCollPlacementInfo =
        getValidatedPlacementInfoForCollection(dbName, collName, [shard0], true);

    // Create a set of zones that will force the new set of chunk to be distributed across all the
    // shards of the cluster.
    const zone1Name = 'zone1';
    assert.commandWorked(st.s.adminCommand({addShardToZone: shard1, zone: zone1Name}));
    const zone1Descriptor = {zone: zone1Name, min: {newShardKey: 0}, max: {newShardKey: 100}};
    const zone2Name = 'zone2';
    assert.commandWorked(st.s.adminCommand({addShardToZone: shard2, zone: zone2Name}));
    const zone2Descriptor = {zone: zone2Name, min: {newShardKey: 200}, max: {newShardKey: 300}};

    // Launch the reshard operation.
    assert.commandWorked(db.adminCommand({
        reshardCollection: nss,
        key: {newShardKey: 1},
        zones: [zone1Descriptor, zone2Descriptor]
    }));

    // A single new placement document should have been added (the temp collection created by
    // resharding does not get tracked in config.placementHistory).
    assert.eq(1 + initialNumPlacementEntries, configDB.placementHistory.count({}));

    // Verify that the latest placement info matches the expectations.
    const finalCollPlacementInfo =
        getValidatedPlacementInfoForCollection(dbName, collName, [shard0, shard1, shard2], false);

    // The resharded collection maintains its nss, but changes its uuid.
    assert.neq(initialCollPlacementInfo.uuid, finalCollPlacementInfo.uuid);
}

function testAddShard() {
    // Create a new replica set and populate it with some initial DBs
    const newReplicaSet = new ReplSetTest({name: "addedShard", nodes: 1});
    const newShardName = 'addedShard';
    const preExistingCollName = 'preExistingColl';
    newReplicaSet.startSet({shardsvr: ""});
    newReplicaSet.initiate();
    const dbsOnNewReplicaSet = ['addShardTestDB1', 'addShardTestDB2'];
    for (const dbName of dbsOnNewReplicaSet) {
        const db = newReplicaSet.getPrimary().getDB(dbName);
        assert.commandWorked(db[preExistingCollName].save({value: 1}));
    }

    // Run addShard(); pre-existing collections in the replica set should be treated as unsharded
    // collections, while their parent DBs should appear in config.placementHistory with consistent
    // details.
    assert.commandWorked(st.s.adminCommand({addShard: newReplicaSet.getURL(), name: newShardName}));

    for (const dbName of dbsOnNewReplicaSet) {
        assert.eq(null, getLatestPlacementInfoFor(dbName + '.' + preExistingCollName));
        const dbPlacementEntry = getValidatedPlacementInfoForDB(dbName);
        assert.sameMembers([newShardName], dbPlacementEntry.shards);
    }

    // Execute the test case teardown
    for (const dbName of dbsOnNewReplicaSet) {
        assert.commandWorked(st.getDB(dbName).dropDatabase());
    }

    let res = assert.commandWorked(st.s.adminCommand({removeShard: newShardName}));
    assert.eq('started', res.state);
    res = assert.commandWorked(st.s.adminCommand({removeShard: newShardName}));
    assert.eq('completed', res.state);
    newReplicaSet.stopSet();
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
testEnableSharding('explicitlyCreatedDB', shard0);

jsTest.log(
    'Testing placement entries added by shardCollection() over an existing sharding-enabled DB');
testShardCollection('explicitlyCreatedDB', 'coll1');

jsTest.log('Testing placement entries added by shardCollection() over a non-existing db (& coll)');
testShardCollection('implicitlyCreatedDB', 'coll1');

jsTest.log('Testing placement entries added by dropCollection()');
testDropCollection();

jsTest.log('Testing placement entries added/not added by a sequence of moveChunk() commands');
testMoveChunk('explicitlyCreatedDB', 'testMoveChunk');

jsTest.log('Testing placement entries added/not added by a sequence of moveRange() commands');
testMoveRange('explicitlyCreatedDB', 'testMoveRange');

jsTest.log(
    'Testing placement entries added by movePrimary() over a new sharding-enabled DB with no data');
testMovePrimary('movePrimaryDB', st.shard0.shardName, st.shard1.shardName);

testRenameCollection();

jsTest.log(
    'Testing placement entries added by dropDatabase() over a new sharding-enabled DB with data');
testDropDatabase('dropDatabaseDB', st.shard0.shardName);

jsTest.log('Testing placement entries added by reshardCollection()');
testReshardCollection();

jsTest.log(
    'Testing placement entries set by addShard() when existing DBs get imported into the cluster');
testAddShard();

st.stop();
}());
