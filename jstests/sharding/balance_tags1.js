// Test balancing all chunks off of one shard
(function() {
    'use strict';

    var st = new ShardingTest({shards: 3, mongos: 1, other: {chunkSize: 1, enableBalancer: true}});

    assert.commandWorked(st.s0.adminCommand({enablesharding: 'test'}));
    st.ensurePrimaryShard('test', 'shard0001');

    var testDB = st.s0.getDB('test');

    var bulk = testDB.foo.initializeUnorderedBulkOp();
    for (var i = 0; i < 21; i++) {
        bulk.insert({_id: i, x: i});
    }
    assert.writeOK(bulk.execute());

    assert.commandWorked(st.s0.adminCommand({shardCollection: 'test.foo', key: {_id: 1}}));

    st.stopBalancer();

    for (var i = 0; i < 20; i++) {
        assert.commandWorked(st.s0.adminCommand({split: 'test.foo', middle: {_id: i}}));
    }

    st.startBalancer();

    st.printShardingStatus(true);

    // Wait for the initial balance to happen
    assert.soon(function() {
        var counts = st.chunkCounts('foo');
        printjson(counts);
        return counts['shard0000'] == 7 && counts['shard0001'] == 7 && counts['shard0002'] == 7;
    }, 'balance 1 did not happen', 1000 * 60 * 10, 1000);

    // Quick test of some shell helpers and setting up state
    st.addShardTag('shard0000', 'a');
    assert.eq(['a'], st.config.shards.findOne({_id: 'shard0000'}).tags);

    st.addShardTag('shard0000', 'b');
    assert.eq(['a', 'b'], st.config.shards.findOne({_id: 'shard0000'}).tags);

    st.removeShardTag('shard0000', 'b');
    assert.eq(['a'], st.config.shards.findOne({_id: 'shard0000'}).tags);

    st.addShardTag('shard0001', 'a');
    st.addTagRange('test.foo', {_id: -1}, {_id: 1000}, 'a');

    st.printShardingStatus(true);

    // At this point, everything should drain off shard 2, which does not have the tag
    assert.soon(function() {
        var counts = st.chunkCounts('foo');
        printjson(counts);
        return counts['shard0002'] == 0;
    }, 'balance 2 did not happen', 1000 * 60 * 10, 1000);

    st.printShardingStatus(true);

    st.stop();
})();
