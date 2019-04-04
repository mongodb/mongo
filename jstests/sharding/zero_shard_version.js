/**
 * Tests the setShardVersion logic on the this shard side, specifically when comparing
 * against a major version of zero or incompatible epochs.
 */
(function() {
    'use strict';

    var st = new ShardingTest({shards: 2, merizos: 4});

    var testDB_s0 = st.s.getDB('test');
    assert.commandWorked(testDB_s0.adminCommand({enableSharding: 'test'}));
    st.ensurePrimaryShard('test', st.shard1.shardName);
    assert.commandWorked(testDB_s0.adminCommand({shardCollection: 'test.user', key: {x: 1}}));

    var checkShardMajorVersion = function(conn, expectedVersion) {
        var shardVersionInfo = conn.adminCommand({getShardVersion: 'test.user'});
        assert.eq(expectedVersion, shardVersionInfo.global.getTime());
    };

    ///////////////////////////////////////////////////////
    // Test shard with empty chunk

    // shard0: 0|0|a
    // shard1: 1|0|a, [-inf, inf)
    // merizos0: 1|0|a

    var testDB_s1 = st.s1.getDB('test');
    assert.writeOK(testDB_s1.user.insert({x: 1}));
    assert.commandWorked(
        testDB_s1.adminCommand({moveChunk: 'test.user', find: {x: 0}, to: st.shard0.shardName}));

    st.configRS.awaitLastOpCommitted();

    // Official config:
    // shard0: 2|0|a, [-inf, inf)
    // shard1: 0|0|a
    //
    // Shard metadata:
    // shard0: 0|0|a
    // shard1: 0|0|a
    // merizos0: 1|0|a

    checkShardMajorVersion(st.rs0.getPrimary(), 0);
    checkShardMajorVersion(st.rs1.getPrimary(), 0);

    // merizos0 still thinks that { x: 1 } belong to st.shard1.shardName, but should be able to
    // refresh it's metadata correctly.
    assert.neq(null, testDB_s0.user.findOne({x: 1}));

    checkShardMajorVersion(st.rs0.getPrimary(), 2);
    checkShardMajorVersion(st.rs1.getPrimary(), 0);

    // Set merizos2 & merizos3 to version 2|0|a
    var testDB_s2 = st.s2.getDB('test');
    assert.neq(null, testDB_s2.user.findOne({x: 1}));

    var testDB_s3 = st.s3.getDB('test');
    assert.neq(null, testDB_s3.user.findOne({x: 1}));

    ///////////////////////////////////////////////////////
    // Test unsharded collection
    // merizos versions: s0, s2, s3: 2|0|a

    testDB_s1.user.drop();
    assert.writeOK(testDB_s1.user.insert({x: 10}));

    // shard0: 0|0|0
    // shard1: 0|0|0
    // merizos0: 2|0|a

    checkShardMajorVersion(st.rs0.getPrimary(), 0);
    checkShardMajorVersion(st.rs1.getPrimary(), 0);

    // merizos0 still thinks { x: 10 } belong to st.shard0.shardName, but since coll is dropped,
    // query should be routed to primary shard.
    assert.neq(null, testDB_s0.user.findOne({x: 10}));

    checkShardMajorVersion(st.rs0.getPrimary(), 0);
    checkShardMajorVersion(st.rs1.getPrimary(), 0);

    ///////////////////////////////////////////////////////
    // Test 2 shards with 1 chunk
    // merizos versions: s0: 0|0|0, s2, s3: 2|0|a

    testDB_s1.user.drop();
    testDB_s1.adminCommand({shardCollection: 'test.user', key: {x: 1}});
    testDB_s1.adminCommand({split: 'test.user', middle: {x: 0}});

    // shard0: 0|0|b,
    // shard1: 1|1|b, [-inf, 0), [0, inf)

    testDB_s1.user.insert({x: 1});
    testDB_s1.user.insert({x: -11});
    assert.commandWorked(
        testDB_s1.adminCommand({moveChunk: 'test.user', find: {x: -1}, to: st.shard0.shardName}));

    st.configRS.awaitLastOpCommitted();

    // Official config:
    // shard0: 2|0|b, [-inf, 0)
    // shard1: 2|1|b, [0, inf)
    //
    // Shard metadata:
    // shard0: 0|0|b
    // shard1: 2|1|b
    //
    // merizos2: 2|0|a

    checkShardMajorVersion(st.rs0.getPrimary(), 0);
    checkShardMajorVersion(st.rs1.getPrimary(), 2);

    // merizos2 still thinks that { x: 1 } belong to st.shard0.shardName, but should be able to
    // refresh it's metadata correctly.
    assert.neq(null, testDB_s2.user.findOne({x: 1}));

    checkShardMajorVersion(st.rs0.getPrimary(), 2);
    checkShardMajorVersion(st.rs1.getPrimary(), 2);

    // Set shard metadata to 2|0|b
    assert.neq(null, testDB_s2.user.findOne({x: -11}));

    checkShardMajorVersion(st.rs0.getPrimary(), 2);
    checkShardMajorVersion(st.rs1.getPrimary(), 2);

    // Official config:
    // shard0: 2|0|b, [-inf, 0)
    // shard1: 2|1|b, [0, inf)
    //
    // Shard metadata:
    // shard0: 2|0|b
    // shard1: 2|1|b
    //
    // merizos3: 2|0|a

    // 4th merizos still thinks that { x: 1 } belong to st.shard0.shardName, but should be able to
    // refresh it's metadata correctly.
    assert.neq(null, testDB_s3.user.findOne({x: 1}));

    ///////////////////////////////////////////////////////
    // Test merizos thinks unsharded when it's actually sharded
    // merizos current versions: s0: 0|0|0, s2, s3: 2|0|b

    // Set merizos0 to version 0|0|0
    testDB_s0.user.drop();

    checkShardMajorVersion(st.rs0.getPrimary(), 0);
    checkShardMajorVersion(st.rs1.getPrimary(), 0);

    assert.eq(null, testDB_s0.user.findOne({x: 1}));

    // Needs to also set merizos1 to version 0|0|0, otherwise it'll complain that collection is
    // already sharded.
    assert.eq(null, testDB_s1.user.findOne({x: 1}));
    assert.commandWorked(testDB_s1.adminCommand({shardCollection: 'test.user', key: {x: 1}}));
    testDB_s1.user.insert({x: 1});

    assert.commandWorked(
        testDB_s1.adminCommand({moveChunk: 'test.user', find: {x: 0}, to: st.shard0.shardName}));

    st.configRS.awaitLastOpCommitted();

    // Official config:
    // shard0: 2|0|c, [-inf, inf)
    // shard1: 0|0|c
    //
    // Shard metadata:
    // shard0: 0|0|c
    // shard1: 0|0|c
    //
    // merizos0: 0|0|0

    checkShardMajorVersion(st.rs0.getPrimary(), 0);
    checkShardMajorVersion(st.rs1.getPrimary(), 0);

    // 1st merizos thinks that collection is unshareded and will attempt to query primary shard.
    assert.neq(null, testDB_s0.user.findOne({x: 1}));

    checkShardMajorVersion(st.rs0.getPrimary(), 2);
    checkShardMajorVersion(st.rs1.getPrimary(), 0);

    st.stop();

})();
