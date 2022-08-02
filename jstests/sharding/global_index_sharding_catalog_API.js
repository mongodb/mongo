/**
 * Tests that the global indexes API correctly creates and drops an index from the catalog.
 *
 * @tags: [multiversion_incompatible, featureFlagGlobalIndexesShardingCatalog]
 */

(function() {
'use strict';

const st = new ShardingTest({mongos: 1, shards: 2});

const shard0 = st.shard0.shardName;
const shard1 = st.shard1.shardName;
const cbName = 'foo';
const collectionName = 'test';
const nss = cbName + '.' + collectionName;
const index1Pattern = {
    x: 1
};
const index1Name = 'x_1';
const index2Pattern = {
    y: 1
};
const index2Name = 'y_1';
const configsvrIndexCatalog = 'config.csrs.indexes';
const configsvrCollectionCatalog = 'config.collections';
const shardIndexCatalog = 'config.shard.indexes';
const shardCollectionCatalog = 'config.shard.collections';

st.s.adminCommand({enableSharding: cbName, primaryShard: shard0});
st.s.adminCommand({shardCollection: nss, key: {_id: 1}});

const collectionUUID = st.s.getCollection('config.collections').findOne({_id: nss}).uuid;
st.rs0.getPrimary().adminCommand({
    _shardsvrRegisterIndex: nss,
    keyPattern: index1Pattern,
    options: {},
    name: index1Name,
    collectionUUID: collectionUUID,
    indexCollectionUUID: UUID(),
    lastmod: Timestamp(0, 0),
    writeConcern: {w: 'majority'}
});

// Check that we created the index on the config server and on shard0, which is the only shard with
// data.
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
assert.eq(0, st.rs1.getPrimary().getCollection(shardIndexCatalog).countDocuments({
    collectionUUID: collectionUUID,
    name: index1Name
}));

// Ensure we committed in the right collection
assert.eq(0, st.configRS.getPrimary().getCollection(shardIndexCatalog).countDocuments({
    collectionUUID: collectionUUID,
    name: index1Name
}));
assert.eq(0, st.rs0.getPrimary().getCollection(configsvrIndexCatalog).countDocuments({
    collectionUUID: collectionUUID,
    name: index1Name
}));
assert.eq(0, st.rs1.getPrimary().getCollection(configsvrIndexCatalog).countDocuments({
    collectionUUID: collectionUUID,
    name: index1Name
}));

st.s.adminCommand({split: nss, middle: {_id: 0}});
st.s.adminCommand({moveChunk: nss, find: {_id: 0}, to: shard1});

st.rs0.getPrimary().adminCommand({
    _shardsvrRegisterIndex: 'foo.test',
    keyPattern: index2Pattern,
    options: {},
    name: index2Name,
    collectionUUID: collectionUUID,
    indexCollectionUUID: UUID(),
    lastmod: Timestamp(0, 0),
    writeConcern: {w: 'majority'}
});

// Check that we created the index on the config server and on both shards because there is data
// everywhere.
assert.eq(1, st.configRS.getPrimary().getCollection(configsvrIndexCatalog).countDocuments({
    collectionUUID: collectionUUID,
    name: index2Name
}));
assert.eq(1, st.rs0.getPrimary().getCollection(shardIndexCatalog).countDocuments({
    collectionUUID: collectionUUID,
    name: index2Name
}));
assert.eq(1, st.rs1.getPrimary().getCollection(shardIndexCatalog).countDocuments({
    collectionUUID: collectionUUID,
    name: index2Name
}));

// Check we didn't commit in a wrong collection.
assert.eq(0, st.configRS.getPrimary().getCollection(shardIndexCatalog).countDocuments({
    collectionUUID: collectionUUID,
    name: index2Name
}));
assert.eq(0, st.rs0.getPrimary().getCollection(configsvrIndexCatalog).countDocuments({
    collectionUUID: collectionUUID,
    name: index2Name
}));
assert.eq(0, st.rs1.getPrimary().getCollection(configsvrIndexCatalog).countDocuments({
    collectionUUID: collectionUUID,
    name: index2Name
}));

// Drop index test.

st.rs0.getPrimary().adminCommand({
    _shardsvrUnregisterIndex: 'foo.test',
    name: index2Name,
    collectionUUID: collectionUUID,
    lastmod: Timestamp(0, 0),
    writeConcern: {w: 'majority'}
});

assert.eq(0, st.configRS.getPrimary().getCollection(configsvrIndexCatalog).countDocuments({
    collectionUUID: collectionUUID,
    name: index2Name
}));
assert.eq(0, st.rs0.getPrimary().getCollection(shardIndexCatalog).countDocuments({
    collectionUUID: collectionUUID,
    name: index2Name
}));
assert.eq(0, st.rs1.getPrimary().getCollection(shardIndexCatalog).countDocuments({
    collectionUUID: collectionUUID,
    name: index2Name
}));

st.stop();
})();
