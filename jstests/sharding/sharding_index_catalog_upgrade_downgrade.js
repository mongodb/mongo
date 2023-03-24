/**
 * Tests that the global indexes collections are dropped on FCV downgrade and recreated after
 * upgrading.
 *
 * @tags: [multiversion_incompatible, featureFlagGlobalIndexesShardingCatalog, requires_fcv_70]
 */

(function() {
'use strict';

const st = new ShardingTest({shards: 1});

const csrsIndexesCollection = 'csrs.indexes';
const shardIndexesCollection = 'shard.indexes';
const shardCollectionsCollection = 'shard.collections';
const csrsCollectionsCollectionNss = 'config.collections';
const shardCollectionsCollectionNss = 'config.' + shardCollectionsCollection;
const nss = 'foo.test';

const CSRSIndexes = st.configRS.getPrimary()
                        .getDB('config')
                        .runCommand({listIndexes: csrsIndexesCollection})
                        .cursor.firstBatch;
assert.eq(3, CSRSIndexes.length);

const shardIndexes = st.rs0.getPrimary()
                         .getDB('config')
                         .runCommand({listIndexes: shardIndexesCollection})
                         .cursor.firstBatch;
assert.eq(3, shardIndexes.length);

const shardCollectionsIndexes = st.rs0.getPrimary()
                                    .getDB('config')
                                    .runCommand({listIndexes: shardCollectionsCollection})
                                    .cursor.firstBatch;
assert.eq(2, shardCollectionsIndexes.length);

st.s.adminCommand({shardCollection: nss, key: {_id: 1}});
const collectionUUID = st.s.getCollection(csrsCollectionsCollectionNss).findOne({_id: nss}).uuid;
st.rs0.getPrimary().adminCommand({
    _shardsvrRegisterIndex: nss,
    keyPattern: {x: 1},
    options: {},
    name: 'x_1',
    collectionUUID: collectionUUID,
    indexCollectionUUID: UUID(),
    lastmod: Timestamp(0, 0),
    writeConcern: {w: 'majority'}
});

assert.eq(1, st.configRS.getPrimary().getCollection(csrsCollectionsCollectionNss).countDocuments({
    uuid: collectionUUID,
    indexVersion: {$exists: true}
}));
assert.eq(1, st.rs0.getPrimary().getCollection(shardCollectionsCollectionNss).countDocuments({
    uuid: collectionUUID,
    indexVersion: {$exists: true}
}));

assert.commandFailedWithCode(st.s.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}),
                             ErrorCodes.CannotDowngrade);
assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV}));

// Drop global index before downgrade
st.rs0.getPrimary().adminCommand({
    _shardsvrUnregisterIndex: nss,
    name: 'x_1',
    collectionUUID: collectionUUID,
    lastmod: Timestamp(0, 0),
    writeConcern: {w: 'majority'}
});

assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));

assert.commandFailedWithCode(
    st.configRS.getPrimary().getDB('config').runCommand({listIndexes: csrsIndexesCollection}),
    ErrorCodes.NamespaceNotFound);

assert.commandFailedWithCode(
    st.rs0.getPrimary().getDB('config').runCommand({listIndexes: shardIndexesCollection}),
    ErrorCodes.NamespaceNotFound);

assert.eq(0, st.configRS.getPrimary().getCollection(csrsCollectionsCollectionNss).countDocuments({
    uuid: collectionUUID,
    indexVersion: {$exists: true}
}));
assert.eq(0, st.rs0.getPrimary().getCollection(shardCollectionsCollectionNss).countDocuments({
    uuid: collectionUUID,
    indexVersion: {$exists: true}
}));
assert.commandFailedWithCode(
    st.rs0.getPrimary().getDB('config').runCommand({listIndexes: shardCollectionsCollection}),
    ErrorCodes.NamespaceNotFound);

st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV});

const afterUpgradeCSRSIndexes = st.configRS.getPrimary()
                                    .getDB('config')
                                    .runCommand({listIndexes: csrsIndexesCollection})
                                    .cursor.firstBatch;
assert.eq(3, afterUpgradeCSRSIndexes.length);

const afterUpgradeShardIndexes = st.rs0.getPrimary()
                                     .getDB('config')
                                     .runCommand({listIndexes: shardIndexesCollection})
                                     .cursor.firstBatch;
assert.eq(3, afterUpgradeShardIndexes.length);

st.stop();
})();
