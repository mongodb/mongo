// This tests sharding an existing collection that both shards are aware of (SERVER-2828)
(function() {
    'use strict';

    var st = new ShardingTest({shards: 2, mongos: 2});

    assert.commandWorked(st.s0.adminCommand({enablesharding: "test"}));
    st.ensurePrimaryShard('test', 'shard0001');
    assert.commandWorked(st.s0.adminCommand({shardcollection: "test.foo", key: {num: 1}}));

    assert.writeOK(st.s0.getDB('test').existing.insert({_id: 1}));
    assert.eq(1, st.s0.getDB('test').existing.count({_id: 1}));
    assert.eq(1, st.s1.getDB('test').existing.count({_id: 1}));

    assert.commandWorked(st.s1.adminCommand({shardcollection: "test.existing", key: {_id: 1}}));
    assert.eq(true, st.s1.getDB('test').existing.stats().sharded);

    assert.commandWorked(st.s1.getDB("admin").runCommand({
        moveChunk: "test.existing",
        find: {_id: 1},
        to: st.getOther(st.getPrimaryShard("test")).name
    }));

    assert.commandWorked(st.s0.adminCommand({flushRouterConfig: 1}));

    assert.eq(1, st.s0.getDB('test').existing.count({_id: 1}));  // SERVER-2828
    assert.eq(1, st.s1.getDB('test').existing.count({_id: 1}));

    st.stop();
})();
