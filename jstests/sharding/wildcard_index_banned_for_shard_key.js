//
// Confirms that a wildcard index cannot be used to support a shard key.
//

(function() {
'use strict';

const st = new ShardingTest({mongos: 1, shards: 2});
const kDbName = 'wildcard_index_banned_for_shard_key';
const mongos = st.s0;

function assertCannotShardCollectionOnWildcardIndex(keyDoc) {
    assert.commandWorked(mongos.adminCommand({enableSharding: kDbName}));

    assert.commandFailedWithCode(
        mongos.adminCommand({shardCollection: `${kDbName}.foo`, key: keyDoc}),
        ErrorCodes.InvalidOptions);

    assert.eq(mongos.getDB('config').collections.count({_id: `${kDbName}.foo`}), 0);
    assert.commandWorked(mongos.getDB(kDbName).dropDatabase());
}

// Can't shard on a path supported by a general wildcard index.
assert.commandWorked(mongos.getDB(kDbName).foo.createIndex({"$**": 1}));
assert.commandWorked(mongos.getDB(kDbName).foo.insert({a: 1}));
assertCannotShardCollectionOnWildcardIndex({a: 1});

// Can't shard on a path supported by a targeted wildcard index.
assert.commandWorked(mongos.getDB(kDbName).foo.createIndex({"a.$**": 1}));
assert.commandWorked(mongos.getDB(kDbName).foo.insert({a: 1}));
assertCannotShardCollectionOnWildcardIndex({a: 1});

// Can't shard on a path supported by wildcard index with projection option.
assert.commandWorked(
    mongos.getDB(kDbName).foo.createIndex({"$**": 1}, {wildcardProjection: {a: 1}}));
assert.commandWorked(mongos.getDB(kDbName).foo.insert({a: 1}));
assertCannotShardCollectionOnWildcardIndex({a: 1});

st.stop();
})();
