/**
 * Tests that the global indexes API correctly creates and drops an index from the catalog.
 *
 * @tags: [multiversion_incompatible, featureFlagGlobalIndexesShardingCatalog]
 */

(function() {
'use strict';

const st = new ShardingTest({mongos: 1, shards: {rs0: {nodes: 3}, rs1: {nodes: 3}}});

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

st.s.adminCommand({enableSharding: cbName, primaryShard: shard0});
st.s.adminCommand({shardCollection: nss, key: {_id: 1}});

const collectionUUID = st.s.getCollection('config.collections').findOne({_id: nss}).uuid;

st.rs0.getPrimary().adminCommand({
    _shardsvrRegisterIndex: nss,
    keyPattern: index1Pattern,
    options: {global: true},
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

// Verify indexes are copied to the new shard after a migration.
assert.eq(1, st.rs1.getPrimary().getCollection(shardIndexCatalog).countDocuments({
    collectionUUID: collectionUUID,
    name: index1Name
}));
assert.eq(1, st.rs1.getSecondary().getCollection(shardIndexCatalog).countDocuments({
    collectionUUID: collectionUUID,
    name: index1Name
}));

// AND the index version.
const indexVersionRS0 = st.rs0.getPrimary()
                            .getDB('config')
                            .shard.collections.findOne({uuid: collectionUUID})
                            .indexVersion;
const indexVersionRS1 = st.rs1.getPrimary()
                            .getDB('config')
                            .shard.collections.findOne({uuid: collectionUUID})
                            .indexVersion;
assert.eq(indexVersionRS0, indexVersionRS1);

st.rs0.getPrimary().adminCommand({
    _shardsvrRegisterIndex: 'foo.test',
    keyPattern: index2Pattern,
    options: {global: true},
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

// Check global index consolidation.
// Case 1: 1 leftover index dropped.
// Initial state: there must be only one index in the shards.
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
st.rs0.getPrimary().adminCommand({
    _shardsvrUnregisterIndex: 'foo.test',
    name: index1Name,
    collectionUUID: collectionUUID,
    lastmod: Timestamp(0, 0),
    writeConcern: {w: 'majority'}
});

// Check that there is leftover data.
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

// Consolidation of indexes for case 1.
st.s.adminCommand({moveChunk: nss, find: {_id: 0}, to: shard1});
assert.eq(0, st.rs0.getPrimary().getCollection(shardIndexCatalog).countDocuments({
    collectionUUID: collectionUUID
}));
assert.eq(0, st.rs1.getPrimary().getCollection(shardIndexCatalog).countDocuments({
    collectionUUID: collectionUUID
}));

// Case 2: 1 leftover index dropped, and another index created.
// Add one index.
st.rs0.getPrimary().adminCommand({
    _shardsvrRegisterIndex: nss,
    keyPattern: index1Pattern,
    options: {global: true},
    name: index1Name,
    collectionUUID: collectionUUID,
    indexCollectionUUID: UUID(),
    lastmod: Timestamp(0, 0),
    writeConcern: {w: 'majority'}
});

// Move only chunk to shard0, leaving "garbage".
st.s.adminCommand({moveChunk: nss, find: {_id: 0}, to: shard0});
assert.eq(1, st.rs1.getPrimary().getCollection(shardIndexCatalog).countDocuments({
    collectionUUID: collectionUUID,
    name: index1Name
}));

// Drop and create another index.
st.rs0.getPrimary().adminCommand({
    _shardsvrUnregisterIndex: 'foo.test',
    name: index1Name,
    collectionUUID: collectionUUID,
    lastmod: Timestamp(0, 0),
    writeConcern: {w: 'majority'}
});
st.rs0.getPrimary().adminCommand({
    _shardsvrRegisterIndex: nss,
    keyPattern: index2Pattern,
    options: {global: true},
    name: index2Name,
    collectionUUID: collectionUUID,
    indexCollectionUUID: UUID(),
    lastmod: Timestamp(0, 0),
    writeConcern: {w: 'majority'}
});

// We'll find leftover index info.
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

// Moving the chunk back, this will consolidate the indexes.
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

// Case 3: Multi-index consolidation test. Create index1 again.
st.rs0.getPrimary().adminCommand({
    _shardsvrRegisterIndex: nss,
    keyPattern: index1Pattern,
    options: {global: true},
    name: index1Name,
    collectionUUID: collectionUUID,
    indexCollectionUUID: UUID(),
    lastmod: Timestamp(0, 0),
    writeConcern: {w: 'majority'}
});

// Move the chunk back and clear the indexes.
st.s.adminCommand({moveChunk: nss, find: {_id: 0}, to: shard0});
st.rs0.getPrimary().adminCommand({
    _shardsvrUnregisterIndex: 'foo.test',
    name: index1Name,
    collectionUUID: collectionUUID,
    lastmod: Timestamp(0, 0),
    writeConcern: {w: 'majority'}
});
st.rs0.getPrimary().adminCommand({
    _shardsvrUnregisterIndex: 'foo.test',
    name: index2Name,
    collectionUUID: collectionUUID,
    lastmod: Timestamp(0, 0),
    writeConcern: {w: 'majority'}
});

// Check for leftover data.
assert.eq(0, st.rs0.getPrimary().getCollection(shardIndexCatalog).countDocuments({
    collectionUUID: collectionUUID
}));
assert.eq(2, st.rs1.getPrimary().getCollection(shardIndexCatalog).countDocuments({
    collectionUUID: collectionUUID
}));

// Create the new indexes.
st.rs0.getPrimary().adminCommand({
    _shardsvrRegisterIndex: nss,
    keyPattern: index3Pattern,
    options: {global: true},
    name: index3Name,
    collectionUUID: collectionUUID,
    indexCollectionUUID: UUID(),
    lastmod: Timestamp(0, 0),
    writeConcern: {w: 'majority'}
});
st.rs0.getPrimary().adminCommand({
    _shardsvrRegisterIndex: nss,
    keyPattern: index4Pattern,
    options: {global: true},
    name: index4Name,
    collectionUUID: collectionUUID,
    indexCollectionUUID: UUID(),
    lastmod: Timestamp(0, 0),
    writeConcern: {w: 'majority'}
});

// Move chunk, it should consolidate the indexes.
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

st.stop();
})();
