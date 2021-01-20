/**
 * This test checks that the timestamp in config server's config.collections and in shards
 * config.cache.collections are updated when sharding a collection, dropping and creating a
 * collection, or refining the sharding key.
 *
 * The test can only run when the featureFlagShardingFullDDLSupportTimestampedVersion feature flag
 * is enabled. Tagging as multiversion_incompatible until SERVER-52588 is done.
 *
 * @tags: [multiversion_incompatible, featureFlagShardingFullDDLSupportTimestampedVersion]
 */

(function() {
'use strict';

function checkTimestampConsistencyInPersistentMetadata(nss, timestampInConfig) {
    // Checking consistency on local shard collection: config.cache.collections
    let timestampInShard =
        st.shard0.getDB('config').cache.collections.findOne({_id: nss}).timestamp;
    assert.neq(null, timestampInShard);
    assert.eq(timestampCmp(timestampInConfig, timestampInShard), 0);

    // Checking consistency on config server collection: config.chunks
    var cursor = st.config.chunks.find({ns: nss});
    assert(cursor.hasNext());
    assert.eq(timestampInConfig, cursor.next().lastmodTimestamp);
}

const kDbName = 'testdb';
const kCollName = 'coll';
const kNs = kDbName + '.' + kCollName;

var st = new ShardingTest({shards: 1, mongos: 1});

const featureFlagParam = assert.commandWorked(st.configRS.getPrimary().adminCommand(
    {getParameter: 1, featureFlagShardingFullDDLSupportTimestampedVersion: 1}));

if (!featureFlagParam.featureFlagShardingFullDDLSupportTimestampedVersion.value) {
    jsTest.log(
        'Skipping test because featureFlagShardingFullDDLSupportTimestampedVersion feature flag is not enabled');
    st.stop();
    return;
}

let csrs_config_db = st.configRS.getPrimary().getDB('config');

assert.commandWorked(st.s.adminCommand({enableSharding: kDbName}));

// Create a sharded collection
assert.commandWorked(st.s.adminCommand({shardCollection: kNs, key: {x: 1}}));

// Check that timestamp is created in ConfigSvr and propagated to the shards.
let coll = csrs_config_db.collections.findOne({_id: kNs});
assert.neq(null, coll.timestamp);
let timestampAfterCreate = coll.timestamp;
checkTimestampConsistencyInPersistentMetadata(kNs, timestampAfterCreate);

// Drop the collection and create it again. Collection timestamp should then be updated.
st.s.getDB(kDbName).coll.drop();
assert.commandWorked(st.s.adminCommand({shardCollection: kNs, key: {x: 1}}));
coll = csrs_config_db.collections.findOne({_id: kNs});
assert.neq(null, coll.timestamp);
let timestampAfterDropCreate = coll.timestamp;
assert.eq(timestampCmp(timestampAfterDropCreate, timestampAfterCreate), 1);
checkTimestampConsistencyInPersistentMetadata(kNs, timestampAfterDropCreate);

// Refine sharding key. Collection timestamp should then be updated.
assert.commandWorked(st.s.getCollection(kNs).createIndex({x: 1, y: 1}));
assert.commandWorked(st.s.adminCommand({refineCollectionShardKey: kNs, key: {x: 1, y: 1}}));
coll = csrs_config_db.collections.findOne({_id: kNs});
assert.neq(null, coll.timestamp);
let timestampAfterRefine = coll.timestamp;
assert.eq(timestampCmp(timestampAfterRefine, timestampAfterDropCreate), 1);
checkTimestampConsistencyInPersistentMetadata(kNs, timestampAfterRefine);

st.stop();
})();
