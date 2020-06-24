//
// Basic tests for reshardCollection.
// @tags: [requires_fcv_46]
//

(function() {
'use strict';

const st = new ShardingTest({mongos: 1, shards: 2});
const kDbName = 'db';
const ns = kDbName + '.foo';
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
assert.commandWorked(mongos.getDB(kDbName).dropDatabase());

assert.commandWorked(mongos.adminCommand({enableSharding: kDbName}));
assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: {_id: 1}}));

// Fail if unique is specified and is true.
assert.commandFailedWithCode(
    mongos.adminCommand({reshardCollection: ns, key: {_id: 1}, unique: true}), ErrorCodes.BadValue);
// Succeed if unique is specified and is false.
assert.commandWorked(mongos.adminCommand({reshardCollection: ns, key: {_id: 1}, unique: false}));
assert.commandWorked(mongos.getDB(kDbName).dropDatabase());

// Succeed if _presetReshardedChunks is provided and test commands are enabled (default).
assert.commandWorked(mongos.adminCommand({enableSharding: kDbName}));
assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: {_id: 1}}));
assert.commandWorked(mongos.adminCommand({
    reshardCollection: ns,
    key: {_id: 1},
    _presetReshardedChunks: [{recipientShardId: st.shard1.shardName, min: {_id: 1}, max: {_id: 2}}]
}));
assert.commandWorked(mongos.getDB(kDbName).dropDatabase());

assert.commandWorked(mongos.adminCommand({enableSharding: kDbName}));
assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: {_id: 1}}));

// Fail if both numInitialChunks and _presetReshardedChunks are provided with correct values and
// test commands are enabled (default).
assert.commandFailedWithCode(mongos.adminCommand({
    reshardCollection: ns,
    key: {_id: 1},
    unique: false,
    collation: {locale: 'simple'},
    numInitialChunks: 2,
    _presetReshardedChunks: [{recipientShardId: st.shard1.shardName, min: {_id: 1}, max: {_id: 2}}]
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
assert.commandWorked(mongos.getDB(kDbName).dropDatabase());

// Succeed if all optional fields and _presetReshardedChunks are provided with correct values and
// test commands are enabled (default).
assert.commandWorked(mongos.adminCommand({enableSharding: kDbName}));
assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: {_id: 1}}));
assert.commandWorked(mongos.adminCommand({
    reshardCollection: ns,
    key: {_id: 1},
    unique: false,
    collation: {locale: 'simple'},
    _presetReshardedChunks: [{recipientShardId: st.shard1.shardName, min: {_id: 1}, max: {_id: 2}}]
}));
assert.commandWorked(mongos.getDB(kDbName).dropDatabase());

st.stop();
})();
