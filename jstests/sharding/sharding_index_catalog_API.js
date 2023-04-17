/**
 * Tests that the global indexes API correctly creates and drops an index from the catalog.
 *
 * @tags: [
 *   multiversion_incompatible,
 *   featureFlagGlobalIndexesShardingCatalog,
 * ]
 */

(function() {
'use strict';

function registerIndex(rs, nss, pattern, name, uuid) {
    rs.getPrimary().adminCommand({
        _shardsvrRegisterIndex: nss,
        keyPattern: pattern,
        options: {global: true},
        name: name,
        collectionUUID: uuid,
        indexCollectionUUID: UUID(),
        lastmod: Timestamp(0, 0), /* Placeholder value, a cluster time will be set by the API. */
        writeConcern: {w: 'majority'}
    });
}

function unregisterIndex(rs, nss, name, uuid) {
    rs.getPrimary().adminCommand({
        _shardsvrUnregisterIndex: nss,
        name: name,
        collectionUUID: uuid,
        lastmod: Timestamp(0, 0), /* Placeholder value, a cluster time will be set by the API. */
        writeConcern: {w: 'majority'}
    });
}

const st = new ShardingTest({mongos: 1, shards: {rs0: {nodes: 3}, rs1: {nodes: 3}}});

const shard0 = st.shard0.shardName;
const shard1 = st.shard1.shardName;
const dbName = 'foo';
const collectionName = 'test';
const collection2Name = 'test2';
const collection3Name = 'test3';
const collection4Name = 'test4';
const collection5Name = 'test5';
const collection6Name = 'test6';
const nss = dbName + '.' + collectionName;
const nss2 = dbName + '.' + collection2Name;
const nss3 = dbName + '.' + collection3Name;
const nss4 = dbName + '.' + collection4Name;
const nss5 = dbName + '.' + collection5Name;
const nss6 = dbName + '.' + collection6Name;
const index1Pattern = {
    x: 1
};
const index1Name = 'x_1';
const index2Pattern = {
    y: 1
};
const index2Name = 'y_1';
const index3Pattern = {
    z: 1
};
const index3Name = 'z_1';
const index4Pattern = {
    w: 1
};
const index4Name = 'w_1';
const configsvrIndexCatalog = 'config.csrs.indexes';
const configsvrCollectionCatalog = 'config.collections';
const shardIndexCatalog = 'config.shard.indexes';
const shardCollectionCatalog = 'config.shard.collections';

st.s.adminCommand({enableSharding: dbName, primaryShard: shard0});
assert.commandWorked(st.s.adminCommand({shardCollection: nss, key: {_id: 1}}));
const collectionUUID = st.s.getCollection('config.collections').findOne({_id: nss}).uuid;

registerIndex(st.rs0, nss, index1Pattern, index1Name, collectionUUID);

jsTestLog(
    "Check that we created the index on the config server and on shard0, which is the only shard with data.");
assert.eq(1, st.configRS.getPrimary().getCollection(configsvrIndexCatalog).countDocuments({
    collectionUUID: collectionUUID,
    name: index1Name
}));
assert.eq(1, st.configRS.getPrimary().getCollection(configsvrCollectionCatalog).countDocuments({
    uuid: collectionUUID,
    indexVersion: {$exists: true}
}));
assert.eq(1, st.rs0.getPrimary().getCollection(shardIndexCatalog).countDocuments({
    collectionUUID: collectionUUID,
    name: index1Name
}));
assert.eq(1, st.rs0.getPrimary().getCollection(shardCollectionCatalog).countDocuments({
    uuid: collectionUUID,
    indexVersion: {$exists: true}
}));
assert.eq(1, st.rs0.getSecondary().getCollection(shardIndexCatalog).countDocuments({
    collectionUUID: collectionUUID,
    name: index1Name
}));
assert.eq(1, st.rs0.getSecondary().getCollection(shardCollectionCatalog).countDocuments({
    uuid: collectionUUID,
    indexVersion: {$exists: true}
}));
assert.eq(0, st.rs1.getPrimary().getCollection(shardIndexCatalog).countDocuments({
    collectionUUID: collectionUUID,
    name: index1Name
}));

jsTestLog("Ensure we committed in the right collection.");
if (TestData.configShard) {
    // The config server is shard0 in config shard mode, so it should have both collections.
    assert.eq(1, st.configRS.getPrimary().getCollection(shardIndexCatalog).countDocuments({
        collectionUUID: collectionUUID,
        name: index1Name
    }));
    assert.eq(1, st.rs0.getPrimary().getCollection(configsvrIndexCatalog).countDocuments({
        collectionUUID: collectionUUID,
        name: index1Name
    }));
} else {
    assert.eq(0, st.configRS.getPrimary().getCollection(shardIndexCatalog).countDocuments({
        collectionUUID: collectionUUID,
        name: index1Name
    }));
    assert.eq(0, st.rs0.getPrimary().getCollection(configsvrIndexCatalog).countDocuments({
        collectionUUID: collectionUUID,
        name: index1Name
    }));
}
assert.eq(0, st.rs1.getPrimary().getCollection(configsvrIndexCatalog).countDocuments({
    collectionUUID: collectionUUID,
    name: index1Name
}));

st.s.adminCommand({split: nss, middle: {_id: 0}});
st.s.adminCommand({moveChunk: nss, find: {_id: 0}, to: shard1});

jsTestLog("Verify indexes are copied to the new shard after a migration.");
assert.eq(1, st.rs1.getPrimary().getCollection(shardIndexCatalog).countDocuments({
    collectionUUID: collectionUUID,
    name: index1Name
}));
assert.eq(1, st.rs1.getSecondary().getCollection(shardIndexCatalog).countDocuments({
    collectionUUID: collectionUUID,
    name: index1Name
}));

jsTestLog("AND the index version.");
const indexVersionRS0 = st.rs0.getPrimary()
                            .getDB('config')
                            .shard.collections.findOne({uuid: collectionUUID})
                            .indexVersion;
const indexVersionRS1 = st.rs1.getPrimary()
                            .getDB('config')
                            .shard.collections.findOne({uuid: collectionUUID})
                            .indexVersion;
assert.eq(indexVersionRS0, indexVersionRS1);

registerIndex(st.rs0, nss, index2Pattern, index2Name, collectionUUID);

jsTestLog(
    "Check that we created the index on the config server and on both shards because there is data everywhere.");
assert.eq(1, st.configRS.getPrimary().getCollection(configsvrIndexCatalog).countDocuments({
    collectionUUID: collectionUUID,
    name: index2Name
}));
assert.eq(1, st.rs0.getPrimary().getCollection(shardIndexCatalog).countDocuments({
    collectionUUID: collectionUUID,
    name: index2Name
}));
assert.eq(1, st.rs0.getSecondary().getCollection(shardIndexCatalog).countDocuments({
    collectionUUID: collectionUUID,
    name: index2Name
}));
assert.eq(1, st.rs0.getSecondary().getCollection(shardCollectionCatalog).countDocuments({
    uuid: collectionUUID,
    indexVersion: {$exists: true}
}));
assert.eq(1, st.rs1.getPrimary().getCollection(shardIndexCatalog).countDocuments({
    collectionUUID: collectionUUID,
    name: index2Name
}));
assert.eq(1, st.rs1.getSecondary().getCollection(shardIndexCatalog).countDocuments({
    collectionUUID: collectionUUID,
    name: index2Name
}));

jsTestLog("Check we didn't commit in a wrong collection.");
if (TestData.configShard) {
    // The config server is shard0 in config shard mode, so it should have both collections.
    assert.eq(1, st.configRS.getPrimary().getCollection(shardIndexCatalog).countDocuments({
        collectionUUID: collectionUUID,
        name: index2Name
    }));
    assert.eq(1, st.rs0.getPrimary().getCollection(configsvrIndexCatalog).countDocuments({
        collectionUUID: collectionUUID,
        name: index2Name
    }));
} else {
    assert.eq(0, st.configRS.getPrimary().getCollection(shardIndexCatalog).countDocuments({
        collectionUUID: collectionUUID,
        name: index2Name
    }));
    assert.eq(0, st.rs0.getPrimary().getCollection(configsvrIndexCatalog).countDocuments({
        collectionUUID: collectionUUID,
        name: index2Name
    }));
}
assert.eq(0, st.rs1.getPrimary().getCollection(configsvrIndexCatalog).countDocuments({
    collectionUUID: collectionUUID,
    name: index2Name
}));

jsTestLog("Drop index test.");
unregisterIndex(st.rs0, nss, index2Name, collectionUUID);

assert.eq(0, st.configRS.getPrimary().getCollection(configsvrIndexCatalog).countDocuments({
    collectionUUID: collectionUUID,
    name: index2Name
}));
assert.eq(0, st.rs0.getPrimary().getCollection(shardIndexCatalog).countDocuments({
    collectionUUID: collectionUUID,
    name: index2Name
}));
assert.eq(0, st.rs0.getSecondary().getCollection(shardIndexCatalog).countDocuments({
    collectionUUID: collectionUUID,
    name: index2Name
}));
assert.eq(0, st.rs1.getPrimary().getCollection(shardIndexCatalog).countDocuments({
    collectionUUID: collectionUUID,
    name: index2Name
}));
assert.eq(0, st.rs1.getSecondary().getCollection(shardIndexCatalog).countDocuments({
    collectionUUID: collectionUUID,
    name: index2Name
}));

jsTestLog(
    "Check global index consolidation. Case 1: 1 leftover index dropped. Initial state: there must be only one index in the shards.");
assert.eq(1, st.rs0.getPrimary().getCollection(shardIndexCatalog).countDocuments({
    collectionUUID: collectionUUID
}));
assert.eq(1, st.rs0.getPrimary().getCollection(shardIndexCatalog).countDocuments({
    collectionUUID: collectionUUID,
    name: index1Name
}));
assert.eq(1, st.rs1.getPrimary().getCollection(shardIndexCatalog).countDocuments({
    collectionUUID: collectionUUID
}));
assert.eq(1, st.rs1.getPrimary().getCollection(shardIndexCatalog).countDocuments({
    collectionUUID: collectionUUID,
    name: index1Name
}));

st.s.adminCommand({moveChunk: nss, find: {_id: 0}, to: shard0});
unregisterIndex(st.rs0, nss, index1Name, collectionUUID);

jsTestLog("Check that there is leftover data.");
assert.eq(0, st.rs0.getPrimary().getCollection(shardIndexCatalog).countDocuments({
    collectionUUID: collectionUUID
}));
assert.eq(1, st.rs1.getPrimary().getCollection(shardIndexCatalog).countDocuments({
    collectionUUID: collectionUUID
}));
assert.eq(1, st.rs1.getPrimary().getCollection(shardIndexCatalog).countDocuments({
    collectionUUID: collectionUUID,
    name: index1Name
}));

jsTestLog("Consolidation of indexes for case 1.");
st.s.adminCommand({moveChunk: nss, find: {_id: 0}, to: shard1});
assert.eq(0, st.rs0.getPrimary().getCollection(shardIndexCatalog).countDocuments({
    collectionUUID: collectionUUID
}));
assert.eq(0, st.rs1.getPrimary().getCollection(shardIndexCatalog).countDocuments({
    collectionUUID: collectionUUID
}));

jsTestLog("Case 2: 1 leftover index dropped, and another index created. Add one index.");
registerIndex(st.rs0, nss, index1Pattern, index1Name, collectionUUID);

jsTestLog("Move only chunk to shard0, leaving 'garbage'.");
st.s.adminCommand({moveChunk: nss, find: {_id: 0}, to: shard0});
assert.eq(1, st.rs1.getPrimary().getCollection(shardIndexCatalog).countDocuments({
    collectionUUID: collectionUUID,
    name: index1Name
}));

jsTestLog("Drop and create another index.");

unregisterIndex(st.rs0, nss, index1Name, collectionUUID);
registerIndex(st.rs0, nss, index2Pattern, index2Name, collectionUUID);

jsTestLog("We'll find leftover index info.");
assert.eq(1, st.rs0.getPrimary().getCollection(shardIndexCatalog).countDocuments({
    collectionUUID: collectionUUID,
    name: index2Name
}));
assert.eq(0, st.rs0.getPrimary().getCollection(shardIndexCatalog).countDocuments({
    collectionUUID: collectionUUID,
    name: index1Name
}));
assert.eq(1, st.rs1.getPrimary().getCollection(shardIndexCatalog).countDocuments({
    collectionUUID: collectionUUID,
    name: index1Name
}));
assert.eq(0, st.rs1.getPrimary().getCollection(shardIndexCatalog).countDocuments({
    collectionUUID: collectionUUID,
    name: index2Name
}));

jsTestLog("Moving the chunk back, this will consolidate the indexes.");
st.s.adminCommand({moveChunk: nss, find: {_id: 0}, to: shard1});
assert.eq(1, st.rs0.getPrimary().getCollection(shardIndexCatalog).countDocuments({
    collectionUUID: collectionUUID,
    name: index2Name
}));
assert.eq(0, st.rs0.getPrimary().getCollection(shardIndexCatalog).countDocuments({
    collectionUUID: collectionUUID,
    name: index1Name
}));
assert.eq(1, st.rs1.getPrimary().getCollection(shardIndexCatalog).countDocuments({
    collectionUUID: collectionUUID,
    name: index2Name
}));
assert.eq(0, st.rs1.getPrimary().getCollection(shardIndexCatalog).countDocuments({
    collectionUUID: collectionUUID,
    name: index1Name
}));

jsTestLog("Case 3: Multi-index consolidation test. Create index1 again.");
registerIndex(st.rs0, nss, index1Pattern, index1Name, collectionUUID);

jsTestLog("Move the chunk back and clear the indexes.");
st.s.adminCommand({moveChunk: nss, find: {_id: 0}, to: shard0});

unregisterIndex(st.rs0, nss, index1Name, collectionUUID);
unregisterIndex(st.rs0, nss, index2Name, collectionUUID);

jsTestLog("Check for leftover data.");
assert.eq(0, st.rs0.getPrimary().getCollection(shardIndexCatalog).countDocuments({
    collectionUUID: collectionUUID
}));
assert.eq(2, st.rs1.getPrimary().getCollection(shardIndexCatalog).countDocuments({
    collectionUUID: collectionUUID
}));

jsTestLog("Create the new indexes.");
registerIndex(st.rs0, nss, index3Pattern, index3Name, collectionUUID);
registerIndex(st.rs0, nss, index4Pattern, index4Name, collectionUUID);

jsTestLog("Move chunk, it should consolidate the indexes.");
st.s.adminCommand({moveChunk: nss, find: {_id: 0}, to: shard1});
assert.eq(2, st.rs0.getPrimary().getCollection(shardIndexCatalog).countDocuments({
    collectionUUID: collectionUUID
}));
assert.eq(2, st.rs1.getPrimary().getCollection(shardIndexCatalog).countDocuments({
    collectionUUID: collectionUUID
}));
assert.eq(1, st.rs0.getPrimary().getCollection(shardIndexCatalog).countDocuments({
    collectionUUID: collectionUUID,
    name: index3Name
}));
assert.eq(1, st.rs0.getPrimary().getCollection(shardIndexCatalog).countDocuments({
    collectionUUID: collectionUUID,
    name: index4Name
}));
assert.eq(1, st.rs1.getPrimary().getCollection(shardIndexCatalog).countDocuments({
    collectionUUID: collectionUUID,
    name: index3Name
}));
assert.eq(1, st.rs1.getPrimary().getCollection(shardIndexCatalog).countDocuments({
    collectionUUID: collectionUUID,
    name: index4Name
}));

jsTestLog(
    "Rename test. Have one sharded collection with indexVersion 1. Have a second collection with indexVersion 2. Rename A to B, indexVersion should be bumped.");
assert.commandWorked(st.s.adminCommand({shardCollection: nss2, key: {_id: 1}}));
const collection2UUID = st.s.getCollection('config.collections').findOne({_id: nss2}).uuid;

registerIndex(st.rs0, nss2, index1Pattern, index1Name, collection2UUID);
registerIndex(st.rs0, nss2, index2Pattern, index2Name, collection2UUID);

assert.eq(2, st.rs0.getPrimary().getCollection(shardIndexCatalog).countDocuments({
    collectionUUID: collection2UUID
}));
assert.eq(1, st.rs0.getPrimary().getCollection(shardIndexCatalog).countDocuments({
    collectionUUID: collection2UUID,
    name: index1Name
}));
assert.eq(1, st.rs0.getPrimary().getCollection(shardIndexCatalog).countDocuments({
    collectionUUID: collection2UUID,
    name: index2Name
}));

jsTestLog(
    "Check correct index metadata after rename. Case 1: rename existing sharded collection with destination sharded collection. Snapshot indexVersions for future checks.");
const indexVersionNss = st.configRS.getPrimary()
                            .getCollection(configsvrCollectionCatalog)
                            .findOne({_id: nss})
                            .indexVersion;
const indexVersionNss2 = st.configRS.getPrimary()
                             .getCollection(configsvrCollectionCatalog)
                             .findOne({_id: nss2})
                             .indexVersion;
jsTestLog(
    "This is the case where the bump makes sense, nss has different indexes than nss2, so, after the rename, we should keep the original indexes of nss.");
assert(timestampCmp(indexVersionNss, indexVersionNss2));

assert.commandWorked(
    st.s.getDB(dbName).adminCommand({renameCollection: nss, to: nss2, dropTarget: true}));

jsTestLog("Check CSRS metadata.");
assert.eq(2, st.configRS.getPrimary().getCollection(configsvrIndexCatalog).countDocuments({
    collectionUUID: collectionUUID,
}));
assert.eq(0, st.configRS.getPrimary().getCollection(configsvrIndexCatalog).countDocuments({
    collectionUUID: collection2UUID,
}));
assert.eq(1, st.configRS.getPrimary().getCollection(configsvrIndexCatalog).countDocuments({
    collectionUUID: collectionUUID,
    name: index3Name
}));
assert.eq(1, st.configRS.getPrimary().getCollection(configsvrIndexCatalog).countDocuments({
    collectionUUID: collectionUUID,
    name: index4Name
}));
jsTestLog("Check RS0 metadata.");
assert.eq(2, st.rs0.getPrimary().getCollection(shardIndexCatalog).countDocuments({
    collectionUUID: collectionUUID
}));
assert.eq(0, st.rs0.getPrimary().getCollection(shardIndexCatalog).countDocuments({
    collectionUUID: collection2UUID
}));
assert.eq(1, st.rs0.getPrimary().getCollection(shardIndexCatalog).countDocuments({
    collectionUUID: collectionUUID,
    name: index3Name
}));
assert.eq(1, st.rs0.getPrimary().getCollection(shardIndexCatalog).countDocuments({
    collectionUUID: collectionUUID,
    name: index4Name
}));
jsTestLog("Check RS1 metadata.");
assert.eq(2, st.rs1.getPrimary().getCollection(shardIndexCatalog).countDocuments({
    collectionUUID: collectionUUID
}));
assert.eq(0, st.rs1.getPrimary().getCollection(shardIndexCatalog).countDocuments({
    collectionUUID: collection2UUID
}));
assert.eq(1, st.rs1.getPrimary().getCollection(shardIndexCatalog).countDocuments({
    collectionUUID: collectionUUID,
    name: index3Name
}));
assert.eq(1, st.rs1.getPrimary().getCollection(shardIndexCatalog).countDocuments({
    collectionUUID: collectionUUID,
    name: index4Name
}));
const indexVersionAfterFirstRename = st.configRS.getPrimary()
                                         .getCollection(configsvrCollectionCatalog)
                                         .findOne({_id: nss2})
                                         .indexVersion;
assert(timestampCmp(indexVersionNss, indexVersionAfterFirstRename) < 0);
assert(timestampCmp(indexVersionNss2, indexVersionAfterFirstRename) < 0);

st.s.getCollection(nss3).insert({x: 1});

jsTestLog("Case 2: Rename from unsharded collection to sharded collection clears the indexes");

assert.commandWorked(
    st.s.getDB(dbName).adminCommand({renameCollection: nss3, to: nss2, dropTarget: true}));

assert.eq(0, st.rs0.getPrimary().getCollection(shardIndexCatalog).countDocuments({
    collectionUUID: collectionUUID
}));
assert.eq(0, st.rs1.getPrimary().getCollection(shardIndexCatalog).countDocuments({
    collectionUUID: collectionUUID
}));
assert.eq(0, st.configRS.getPrimary().getCollection(configsvrIndexCatalog).countDocuments({
    collectionUUID: collectionUUID,
}));

assert.commandWorked(st.s.getDB(dbName).runCommand({drop: collection2Name}));

jsTestLog(
    "Case 3: Rename from sharded collection without indexes to sharded collection with indexes clears the indexVersion");

assert.commandWorked(st.s.adminCommand({shardCollection: nss2, key: {_id: 1}}));
assert.commandWorked(st.s.adminCommand({shardCollection: nss3, key: {_id: 1}}));

let collection3UUID = st.s.getCollection('config.collections').findOne({_id: nss3}).uuid;

registerIndex(st.rs0, nss3, index1Pattern, index1Name, collection3UUID);

assert.commandWorked(
    st.s.getDB(dbName).adminCommand({renameCollection: nss2, to: nss3, dropTarget: true}));

let nss3Metadata =
    st.configRS.getPrimary().getCollection(configsvrCollectionCatalog).findOne({_id: nss3});
assert(!nss3Metadata.indexVersion);

jsTestLog("Drop collection test. Create a sharded collection and some indexes");
assert.commandWorked(st.s.adminCommand({shardCollection: nss4, key: {_id: 1}}));
const collection4UUID = st.s.getCollection('config.collections').findOne({_id: nss4}).uuid;

registerIndex(st.rs0, nss4, index1Pattern, index1Name, collection4UUID);
registerIndex(st.rs0, nss4, index2Pattern, index2Name, collection4UUID);

assert.eq(2, st.rs0.getPrimary().getCollection(shardIndexCatalog).countDocuments({
    collectionUUID: collection4UUID
}));
assert.eq(2, st.configRS.getPrimary().getCollection(configsvrIndexCatalog).countDocuments({
    collectionUUID: collection4UUID,
}));
jsTestLog("Drop collection, should eliminate indexes everywhere.");
assert.commandWorked(st.s.getDB(dbName).runCommand({drop: collection4Name}));
assert.eq(0, st.rs0.getPrimary().getCollection(shardIndexCatalog).countDocuments({
    collectionUUID: collection4UUID
}));
assert.eq(0, st.configRS.getPrimary().getCollection(configsvrIndexCatalog).countDocuments({
    collectionUUID: collection4UUID,
}));

jsTestLog("Drop database test. Create a sharded collection and some indexes");
assert.commandWorked(st.s.adminCommand({shardCollection: nss5, key: {_id: 1}}));

const collection5UUID = st.s.getCollection('config.collections').findOne({_id: nss5}).uuid;

registerIndex(st.rs0, nss5, index3Pattern, index3Name, collection5UUID);
registerIndex(st.rs0, nss5, index4Pattern, index4Name, collection5UUID);

assert.eq(2, st.rs0.getPrimary().getCollection(shardIndexCatalog).countDocuments({
    collectionUUID: collection5UUID
}));
assert.eq(2, st.configRS.getPrimary().getCollection(configsvrIndexCatalog).countDocuments({
    collectionUUID: collection5UUID,
}));
jsTestLog("Drop database, should eliminate indexes everywhere.");
assert.commandWorked(st.s.getDB(dbName).dropDatabase());
assert.eq(0, st.rs0.getPrimary().getCollection(shardIndexCatalog).countDocuments({
    collectionUUID: collection5UUID
}));
assert.eq(0, st.configRS.getPrimary().getCollection(configsvrIndexCatalog).countDocuments({
    collectionUUID: collection5UUID,
}));

jsTestLog("Resharding Case 1: happy path.");
st.s.adminCommand({enableSharding: dbName, primaryShard: shard0});
assert.eq(0, st.configRS.getPrimary().getCollection(configsvrIndexCatalog).countDocuments({}));
assert.eq(0, st.rs0.getPrimary().getCollection(shardIndexCatalog).countDocuments({}));
assert.commandWorked(st.s.adminCommand({shardCollection: nss6, key: {_id: 1}}));

assert.commandWorked(st.s.adminCommand({split: nss6, middle: {_id: 0}}));
assert.commandWorked(st.s.adminCommand({moveChunk: nss6, find: {_id: 0}, to: shard1}));

const collection6UUID = st.s.getCollection(configsvrCollectionCatalog).findOne({_id: nss6}).uuid;
registerIndex(st.rs0, nss6, index1Pattern, index1Name, collection6UUID);
registerIndex(st.rs0, nss6, index2Pattern, index2Name, collection6UUID);

const beforeReshardingIndexVersion =
    st.s.getCollection(configsvrCollectionCatalog).findOne({_id: nss6}).indexVersion;

assert.commandWorked(st.s.getDB(dbName).runCommand(
    {createIndexes: collection6Name, indexes: [{key: {x: 1}, name: 'x_1'}]}));

jsTestLog("Give enough cardinality for two chunks to resharding.");
assert.commandWorked(st.s.getCollection(nss6).insert({x: 0}));
assert.commandWorked(st.s.getCollection(nss6).insert({x: 1}));

assert.commandWorked(st.s.adminCommand({reshardCollection: nss6, key: {x: 1}}));
const collection6AfterResharding =
    st.s.getCollection(configsvrCollectionCatalog).findOne({_id: nss6});

assert.gte(collection6AfterResharding.indexVersion, beforeReshardingIndexVersion);
assert.eq(
    st.rs0.getPrimary().getCollection(shardCollectionCatalog).findOne({_id: nss6}).indexVersion,
    collection6AfterResharding.indexVersion);
assert.eq(
    st.rs1.getPrimary().getCollection(shardCollectionCatalog).findOne({_id: nss6}).indexVersion,
    collection6AfterResharding.indexVersion);

assert.eq(0, st.configRS.getPrimary().getCollection(configsvrIndexCatalog).countDocuments({
    collectionUUID: collection6UUID
}));
assert.eq(2, st.configRS.getPrimary().getCollection(configsvrIndexCatalog).countDocuments({
    collectionUUID: collection6AfterResharding.uuid
}));

assert.eq(0, st.rs0.getPrimary().getCollection(shardIndexCatalog).countDocuments({
    collectionUUID: collection6UUID
}));

assert.eq(2, st.rs0.getPrimary().getCollection(shardIndexCatalog).countDocuments({
    collectionUUID: collection6AfterResharding.uuid
}));

assert.eq(0, st.rs1.getPrimary().getCollection(shardIndexCatalog).countDocuments({
    collectionUUID: collection6UUID
}));

assert.eq(2, st.rs1.getPrimary().getCollection(shardIndexCatalog).countDocuments({
    collectionUUID: collection6AfterResharding.uuid
}));

jsTestLog("Case 2: donor shard different to destination shard. First move chunks to " + shard1 +
          ".");

let coll6Chunks =
    st.s.getCollection('config.chunks').find({uuid: collection6AfterResharding.uuid}).toArray();
let newChunks = [];

coll6Chunks.forEach((chunk) => {
    assert.commandWorked(st.s.adminCommand({moveChunk: nss6, find: chunk.min, to: shard1}));
    newChunks.push({min: {y: chunk.min.x}, max: {y: chunk.max.x}, recipientShardId: shard0});
});

assert.eq(2, st.rs1.getPrimary().getCollection(shardIndexCatalog).countDocuments({
    collectionUUID: collection6AfterResharding.uuid
}));

assert.commandWorked(st.s.getDB(dbName).runCommand(
    {createIndexes: collection6Name, indexes: [{key: {y: 1}, name: 'y_1'}]}));
assert.commandWorked(st.s.getCollection(nss6).insert({y: 0}));
assert.commandWorked(st.s.getCollection(nss6).insert({y: 1}));

assert.commandWorked(
    st.s.adminCommand({reshardCollection: nss6, key: {y: 1}, _presetReshardedChunks: newChunks}));

const collection6AfterSecondResharding =
    st.s.getCollection(configsvrCollectionCatalog).findOne({_id: nss6});

assert.eq(0, st.rs1.getPrimary().getCollection(shardIndexCatalog).countDocuments({
    collectionUUID: collection6AfterSecondResharding.uuid
}));

assert.eq(2, st.rs0.getPrimary().getCollection(shardIndexCatalog).countDocuments({
    collectionUUID: collection6AfterSecondResharding.uuid
}));

st.stop();
})();
