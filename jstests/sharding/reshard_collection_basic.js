//
// Basic tests for reshardCollection.
// @tags: [requires_fcv_47]
//

(function() {
'use strict';

const st = new ShardingTest({mongos: 1, shards: 2});
const kDbName = 'db';
const collName = '.foo';
const ns = kDbName + collName;
const mongos = st.s0;

// Fail if sharding is disabled.
assert.commandFailedWithCode(mongos.adminCommand({reshardCollection: ns, key: {_id: 1}}),
                             ErrorCodes.NamespaceNotFound);

// Fail if collection is unsharded.
assert.commandFailedWithCode(mongos.adminCommand({reshardCollection: ns, key: {_id: 1}}),
                             ErrorCodes.NamespaceNotFound);

assert.commandWorked(mongos.adminCommand({enableSharding: kDbName}));
assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: {_id: 1}}));

// Fail if missing required key.
assert.commandFailedWithCode(mongos.adminCommand({reshardCollection: ns}), 40414);

// Fail if collation is specified and is not {locale: 'simple'}.
assert.commandFailedWithCode(
    mongos.adminCommand({reshardCollection: ns, key: {_id: 1}, collation: {locale: 'en_US'}}),
    ErrorCodes.BadValue);

// Succeed when correct locale is provided.
assert.commandWorked(
    mongos.adminCommand({reshardCollection: ns, key: {_id: 1}, collation: {locale: 'simple'}}));

mongos.getDB(kDbName).foo.drop();

assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: {_id: 1}}));

// Fail if unique is specified and is true.
assert.commandFailedWithCode(
    mongos.adminCommand({reshardCollection: ns, key: {_id: 1}, unique: true}), ErrorCodes.BadValue);
// Succeed if unique is specified and is false.
assert.commandWorked(mongos.adminCommand({reshardCollection: ns, key: {_id: 1}, unique: false}));
mongos.getDB(kDbName).foo.drop();

// Succeed if _presetReshardedChunks is provided and test commands are enabled (default).
assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: {_id: 1}}));
assert.commandWorked(mongos.adminCommand({
    reshardCollection: ns,
    key: {_id: 1},
    _presetReshardedChunks:
        [{recipientShardId: st.shard1.shardName, min: {_id: MinKey}, max: {_id: MaxKey}}]
}));
mongos.getDB(kDbName).foo.drop();

assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: {_id: 1}}));

// Fail if both numInitialChunks and _presetReshardedChunks are provided.
assert.commandFailedWithCode(mongos.adminCommand({
    reshardCollection: ns,
    key: {_id: 1},
    unique: false,
    collation: {locale: 'simple'},
    numInitialChunks: 2,
    _presetReshardedChunks: [
        {recipientShardId: st.shard0.shardName, min: {_id: MinKey}, max: {_id: 0}},
        {recipientShardId: st.shard1.shardName, min: {_id: 0}, max: {_id: MaxKey}}
    ]
}),
                             ErrorCodes.BadValue);

// Succeed if all optional fields and numInitialChunks are provided with correct values.
assert.commandWorked(mongos.adminCommand({
    reshardCollection: ns,
    key: {_id: 1},
    unique: false,
    collation: {locale: 'simple'},
    numInitialChunks: 2,
}));
mongos.getDB(kDbName).foo.drop();

// Succeed if all optional fields and _presetReshardedChunks are provided with correct values and
// test commands are enabled (default).
assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: {_id: 1}}));
assert.commandWorked(mongos.adminCommand({
    reshardCollection: ns,
    key: {_id: 1},
    unique: false,
    collation: {locale: 'simple'},
    _presetReshardedChunks: [
        {recipientShardId: st.shard1.shardName, min: {_id: 0}, max: {_id: MaxKey}},
        {recipientShardId: st.shard0.shardName, min: {_id: MinKey}, max: {_id: 0}}
    ]
}));
mongos.getDB(kDbName).foo.drop();

const existingZoneName = 'x1';

// Fail if authoritative tags exist in config.tags collection and zones are not provided.
assert.commandWorked(
    st.s.adminCommand({addShardToZone: st.shard1.shardName, zone: existingZoneName}));
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));
assert.commandWorked(st.s.adminCommand(
    {updateZoneKeyRange: ns, min: {_id: 0}, max: {_id: 5}, zone: existingZoneName}));

assert.commandFailedWithCode(mongos.adminCommand({
    reshardCollection: ns,
    key: {_id: 1},
    unique: false,
    collation: {locale: 'simple'},
    numInitialChunks: 2,
}),
                             ErrorCodes.BadValue);

// Fail if authoritative tags exist in config.tags collection and zones are provided and use a name
// which does not exist in authoritative tags.
assert.commandFailedWithCode(mongos.adminCommand({
    reshardCollection: ns,
    key: {_id: 1},
    unique: false,
    collation: {locale: 'simple'},
    zones: [{tag: 'x', min: {_id: 5}, max: {_id: 10}, ns: ns}],
    numInitialChunks: 2,
}),
                             ErrorCodes.BadValue);

// Succeed if authoritative tags exist in config.tags collection and zones are provided and use an
// existing zone's name.
assert.commandWorked(mongos.adminCommand({
    reshardCollection: ns,
    key: {_id: 1},
    unique: false,
    collation: {locale: 'simple'},
    zones: [{tag: existingZoneName, min: {_id: 5}, max: {_id: 10}, ns: ns}],
    _presetReshardedChunks: [
        {recipientShardId: st.shard1.shardName, min: {_id: 0}, max: {_id: MaxKey}},
        {recipientShardId: st.shard0.shardName, min: {_id: MinKey}, max: {_id: 0}}
    ]
}));

assert.commandWorked(mongos.getDB(kDbName).dropDatabase());

// TODO remove test case below once the resharding command actually writes to config.collections
// itself Test that shards correctly cache 'reshardingFields' in config.cache.collections
assert.commandWorked(mongos.adminCommand({enableSharding: kDbName}));
st.ensurePrimaryShard(kDbName, st.shard0.shardName);
assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: {_id: 1}}));

let donorReshardingFields = {
    "uuid": UUID(),
    "state": "initializing",
    "donorFields": {"reshardingKey": {x: 1}}
};
assert.commandWorked(st.configRS.getPrimary().getDB("config").collections.update(
    {_id: ns}, {"$set": {"reshardingFields": donorReshardingFields}}));

// Run moveChunk to force shard0 to pick up the new info in config.collections
assert.commandWorked(mongos.adminCommand({moveChunk: ns, find: {_id: 0}, to: st.shard1.shardName}));

let cachedEntry = st.rs0.getPrimary().getDB('config').cache.collections.findOne({_id: ns});
assert.docEq(cachedEntry.reshardingFields, donorReshardingFields);

assert.commandWorked(mongos.getDB(kDbName).dropDatabase());
st.stop();
})();
