// Test chunk migration from collections with V2 indexes.

(function() {
    'use strict';
    load('jstests/libs/get_index_helpers.js');
    load('jstests/libs/override_methods/multiversion_override_balancer_control.js');
    const latest = 'latest';
    const downgrade = '3.2';
    var testDb = 'test';

    //
    // Ensure that we can successfully migrate a chunk of a collection with a V2 index between 3.4
    // shards with featureCompatibilityVersion 3.2.
    //

    // Create a 3.4 sharded cluster.
    var st = new ShardingTest({mongos: 1, shards: 2});
    var shard0 = st.shard0.shardName;
    var shard1 = st.shard1.shardName;

    // Create a sharded collection with a V2 _id index and a V2 non-_id index.
    assert.commandWorked(st.s.adminCommand({enableSharding: testDb}));
    st.ensurePrimaryShard(testDb, shard1);
    assert.commandWorked(
        st.s.getDB(testDb).createCollection('foo', {idIndex: {key: {_id: 1}, name: '_id_', v: 2}}));
    assert.commandWorked(st.s.getDB(testDb).foo.createIndex({a: 1}, {v: 2}));
    assert.commandWorked(st.s.adminCommand({shardCollection: testDb + '.foo', key: {a: 1}}));
    var indexSpec = GetIndexHelpers.findByName(st.shard1.getDB(testDb).foo.getIndexes(), '_id_');
    assert.neq(null, indexSpec);
    assert.eq(2, indexSpec.v, tojson(indexSpec));
    indexSpec = GetIndexHelpers.findByName(st.shard1.getDB(testDb).foo.getIndexes(), 'a_1');
    assert.neq(null, indexSpec);
    assert.eq(2, indexSpec.v, tojson(indexSpec));

    // Successfully migrate a chunk from the collection with V2 indexes between shards.
    assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: '3.2'}));
    assert.commandWorked(st.s.adminCommand({moveChunk: testDb + '.foo', find: {a: 1}, to: shard0}));

    // Make sure the index is V2 on the destination shard.
    indexSpec = GetIndexHelpers.findByName(st.shard0.getDB(testDb).foo.getIndexes(), '_id_');
    assert.neq(null, indexSpec);
    assert.eq(2, indexSpec.v, tojson(indexSpec));
    indexSpec = GetIndexHelpers.findByName(st.shard0.getDB(testDb).foo.getIndexes(), 'a_1');
    assert.neq(null, indexSpec);
    assert.eq(2, indexSpec.v, tojson(indexSpec));
    st.stop();

    //
    // Ensure that we fail to migrate a chunk of a collection with a V2 _id index from a 3.4 mongod
    // shard to a 3.2 mongod shard.
    //

    // Create a 3.4 mongod with a collection that has a V2 _id index.
    var chunkSource = MongoRunner.runMongod({shardsvr: '', binVersion: latest});
    assert.neq(null, chunkSource);
    assert.commandWorked(chunkSource.adminCommand({setFeatureCompatibilityVersion: '3.4'}));
    assert.commandWorked(chunkSource.getDB(testDb).createCollection(
        'foo', {idIndex: {key: {_id: 1}, name: '_id_', v: 2}}));
    assert.commandWorked(chunkSource.getDB(testDb).foo.createIndex({a: 1}, {v: 1}));
    indexSpec = GetIndexHelpers.findByName(chunkSource.getDB(testDb).foo.getIndexes(), '_id_');
    assert.neq(null, indexSpec);
    assert.eq(2, indexSpec.v, tojson(indexSpec));
    indexSpec = GetIndexHelpers.findByName(chunkSource.getDB(testDb).foo.getIndexes(), 'a_1');
    assert.neq(null, indexSpec);
    assert.eq(1, indexSpec.v, tojson(indexSpec));

    // Set mongod's featureCompatibilityVersion to 3.2 so that we can add it to a 3.2 cluster.
    assert.commandWorked(chunkSource.adminCommand({setFeatureCompatibilityVersion: '3.2'}));

    // Create a 3.2 sharded cluster with one shard and add the 3.4 mongod to it.
    st = new ShardingTest({
        shards: 1,
        other: {mongosOptions: {binVersion: downgrade}, shardOptions: {binVersion: downgrade}}
    });
    assert.commandWorked(st.s.adminCommand({addShard: chunkSource.name}));
    assert.commandWorked(st.s.adminCommand({enableSharding: testDb}));
    st.ensurePrimaryShard(testDb, shard1);
    assert.commandWorked(st.s.adminCommand({shardCollection: testDb + '.foo', key: {a: 1}}));

    // Fail to migrate a chunk of the collection with a V2 _id index to the 3.2 shard.
    assert.commandFailedWithCode(
        st.s.adminCommand({moveChunk: testDb + '.foo', find: {a: 1}, to: shard0}),
        ErrorCodes.OperationFailed,
        'Move chunk unexpectedly succeeded. Expected to fail since 3.2 cannot build V2 indexes.');
    st.stop();

    //
    // Ensure that we fail to migrate a chunk of a collection with a V2 non-_id index from a 3.4
    // mongod shard to a 3.2 mongod shard.
    //

    // Create a 3.4 mongod with a collection that has a V2 non-_id index.
    chunkSource = MongoRunner.runMongod({shardsvr: '', binVersion: latest});
    assert.neq(null, chunkSource);
    assert.commandWorked(chunkSource.adminCommand({setFeatureCompatibilityVersion: '3.4'}));
    assert.commandWorked(chunkSource.getDB(testDb).createCollection(
        'foo', {idIndex: {key: {_id: 1}, name: '_id_', v: 1}}));
    assert.commandWorked(chunkSource.getDB(testDb).foo.createIndex({a: 1}, {v: 2}));
    indexSpec = GetIndexHelpers.findByName(chunkSource.getDB(testDb).foo.getIndexes(), '_id_');
    assert.neq(null, indexSpec);
    assert.eq(1, indexSpec.v, tojson(indexSpec));
    indexSpec = GetIndexHelpers.findByName(chunkSource.getDB(testDb).foo.getIndexes(), 'a_1');
    assert.neq(null, indexSpec);
    assert.eq(2, indexSpec.v, tojson(indexSpec));

    // Set mongod's featureCompatibilityVersion to 3.2 so that we can add it to a 3.2 cluster.
    assert.commandWorked(chunkSource.adminCommand({setFeatureCompatibilityVersion: '3.2'}));

    // Create a 3.2 sharded cluster with one shard and add the 3.4 mongod to it.
    st = new ShardingTest({
        shards: 1,
        other: {mongosOptions: {binVersion: downgrade}, shardOptions: {binVersion: downgrade}}
    });
    assert.commandWorked(st.s.adminCommand({addShard: chunkSource.name}));
    assert.commandWorked(st.s.adminCommand({enableSharding: testDb}));
    st.ensurePrimaryShard(testDb, shard1);
    assert.commandWorked(st.s.adminCommand({shardCollection: testDb + '.foo', key: {a: 1}}));

    // Fail to migrate a chunk of the collection with a V2 non-_id index to the 3.2 shard.
    assert.commandFailedWithCode(
        st.s.adminCommand({moveChunk: testDb + '.foo', find: {a: 1}, to: shard0}),
        ErrorCodes.OperationFailed,
        'Move chunk unexpectedly succeeded. Expected to fail since 3.2 cannot build V2 indexes.');
    st.stop();
})();
