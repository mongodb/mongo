/**
 * Tests that the global indexes collections are dropped on FCV downgrade and recreated after
 * upgrading.
 *
 * @tags: [multiversion_incompatible, featureFlagGlobalIndexesShardingCatalog]
 */

(function() {
'use strict';

const st = new ShardingTest({shards: 1});

const csrsIndexesCollection = 'csrs.indexes';
const shardIndexesCollection = 'shard.indexes';

const CSRSIndexes = st.configRS.getPrimary()
                        .getDB('config')
                        .runCommand({listIndexes: csrsIndexesCollection})
                        .cursor.firstBatch;
assert.eq(2, CSRSIndexes.length);

const shardIndexes = st.rs0.getPrimary()
                         .getDB('config')
                         .runCommand({listIndexes: shardIndexesCollection})
                         .cursor.firstBatch;
assert.eq(2, shardIndexes.length);

st.s.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV});

assert.commandFailedWithCode(
    st.configRS.getPrimary().getDB('config').runCommand({listIndexes: csrsIndexesCollection}),
    ErrorCodes.NamespaceNotFound);

assert.commandFailedWithCode(
    st.rs0.getPrimary().getDB('config').runCommand({listIndexes: shardIndexesCollection}),
    ErrorCodes.NamespaceNotFound);

st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV});

const afterUpgradeCSRSIndexes = st.configRS.getPrimary()
                                    .getDB('config')
                                    .runCommand({listIndexes: csrsIndexesCollection})
                                    .cursor.firstBatch;
assert.eq(2, afterUpgradeCSRSIndexes.length);

const afterUpgradeShardIndexes = st.rs0.getPrimary()
                                     .getDB('config')
                                     .runCommand({listIndexes: shardIndexesCollection})
                                     .cursor.firstBatch;
assert.eq(2, afterUpgradeShardIndexes.length);

st.stop();
})();
