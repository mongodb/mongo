/**
 * Test setFCV interactions with per-collection balancing settings
 *
 * @tags: [
 *  multiversion_incompatible,
 *  requires_fcv_53,
 *  featureFlagPerCollBalancingSettings,
 * ]
 */
// TODO SERVER-62693 get rid of this file once 6.0 branches out

'use strict';

const st = new ShardingTest({mongos: 1, shards: 1, other: {enableBalancer: false}});

const database = st.getDB('test');
assert.commandWorked(st.s.adminCommand({enableSharding: 'test'}));
const collName = 'coll';
const coll = database[collName];
const fullNs = coll.getFullName();

assert.commandWorked(st.s.adminCommand({shardCollection: fullNs, key: {x: 1}}));

const downgradeVersion = lastLTSFCV;
assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV}));

/* Test that
 * - downgrade can be performed while a collection is undergoing defragmentation
 * - at the end of the process,  per-collection balancing fields are removed upon setFCV < 5.3
 */
{
    assert.commandWorked(st.s.adminCommand({
        configureCollectionBalancing: fullNs,
        defragmentCollection: true,
        enableAutoSplitter: false,
        chunkSize: 10
    }));

    var configEntryBeforeSetFCV =
        st.config.getSiblingDB('config').collections.findOne({_id: fullNs});
    var shardEntryBeforeSetFCV = st.shard0.getDB('config').cache.collections.findOne({_id: fullNs});
    assert.eq(10 * 1024 * 1024, configEntryBeforeSetFCV.maxChunkSizeBytes);
    assert(configEntryBeforeSetFCV.noAutoSplit);
    assert.eq(10 * 1024 * 1024, shardEntryBeforeSetFCV.maxChunkSizeBytes);
    assert(!shardEntryBeforeSetFCV.allowAutoSplit);
    assert(configEntryBeforeSetFCV.defragmentCollection);

    assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: downgradeVersion}));

    var configEntryAfterSetFCV =
        st.config.getSiblingDB('config').collections.findOne({_id: fullNs});
    var shardEntryAfterSetFCV = st.shard0.getDB('config').cache.collections.findOne({_id: fullNs});
    assert.isnull(configEntryAfterSetFCV.maxChunkSizeBytes);
    assert.isnull(configEntryAfterSetFCV.noAutoSplit);
    assert.isnull(shardEntryAfterSetFCV.maxChunkSizeBytes);
    assert.isnull(shardEntryAfterSetFCV.allowAutoSplit);
}

st.stop();
