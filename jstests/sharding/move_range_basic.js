/*
 * Basic tests for moveRange.
 *
 * @tags: [
 *    featureFlagNoMoreAutoSplitter,
 * ]
 */
'use strict';

load('jstests/sharding/libs/find_chunks_util.js');

var st = new ShardingTest({mongos: 1, shards: 2});
var kDbName = 'db';

var mongos = st.s0;
var shard0 = st.shard0.shardName;
var shard1 = st.shard1.shardName;

assert.commandWorked(mongos.adminCommand({enableSharding: kDbName, primaryShard: shard0}));

function test(collName, shardKey) {
    var ns = kDbName + '.' + collName;
    assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: shardKey}));

    var aChunk = findChunksUtil.findOneChunkByNs(mongos.getDB('config'), ns, {shard: shard0});
    assert(aChunk);

    // Fail if one of the bounds is not a valid shard key
    assert.commandFailed(mongos.adminCommand(
        {moveRange: ns, min: aChunk.min, max: {invalidShardKey: 10}, toShard: shard1}));

    // Fail if the `to` shard does not exists
    assert.commandFailed(mongos.adminCommand(
        {moveRange: ns, min: aChunk.min, max: aChunk.max, toShard: 'WrongShard'}));

    assert.commandWorked(
        mongos.adminCommand({moveRange: ns, min: aChunk.min, max: aChunk.max, toShard: shard1}));

    assert.eq(0, mongos.getDB('config').chunks.count({_id: aChunk._id, shard: shard0}));
    assert.eq(1, mongos.getDB('config').chunks.count({_id: aChunk._id, shard: shard1}));
}

test('nonHashedShardKey', {a: 1});

test('nonHashedCompundShardKey', {a: 1, b: 1});

test('hashedShardKey', {_id: 'hashed'});

st.stop();
