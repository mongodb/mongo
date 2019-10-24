/**
 * TODO: SERVER-44105 maybe remove this test.
 * This test is a simplified version that tries to simulate the condition described in
 * SERVER-42737. It is very hard to replicate the exact condition because of:
 *
 * https://github.com/mongodb/mongo/blob/r4.3.0/src/mongo/db/s/shard_server_catalog_cache_loader.cpp#L293
 *
 * This means that secondary should be replicating a newer refresh after that line
 * above and hit the condition described in the ticket to hit the bug.
 */
(function() {
    let rsOptions = {nodes: 2};
    let st = new ShardingTest({shards: {rs0: rsOptions, rs1: rsOptions}});

    assert.commandWorked(st.s.adminCommand({enableSharding: 'test'}));
    st.ensurePrimaryShard('test', st.shard0.shardName);
    assert.commandWorked(st.s.adminCommand({shardCollection: 'test.user', key: {x: 1}}));
    assert.commandWorked(st.s.adminCommand({split: 'test.user', middle: {x: 0}}));

    let coll = st.s.getDB('test').user;

    assert.commandWorked(coll.insert({x: -1}));
    assert.commandWorked(coll.insert({x: 1}));

    assert.commandWorked(
        st.s.adminCommand({moveChunk: 'test.user', find: {x: 0}, to: st.shard1.shardName}));

    // Manually set refreshing flag to true so secondary cache refresh will block.

    st.rs0.getPrimary().getDB('config').cache.collections.update(
        {_id: 'test.user', fake: {'$exists': false}}, {$set: {refreshing: true}});

    // Add a delay (sleep) to make sure that secondary will have the {refreshing: true}
    // in the lastApplied snapshot and is blocked waiting for refreshing to become false
    // before sending update to primary.

    let joinUpdate = startParallelShell(
        'sleep(1000);' +
            'db.getSiblingDB("config").cache.collections.update(' +
            '{_id: "test.user", fake: {"$exists": false}},' +
            '{$set: {refreshing: false, lastRefreshedCollectionVersion: Timestamp(5, 0)}});',
        st.rs0.getPrimary().port);

    // This secondary read should not cause a hang.
    st.s.setReadPref('secondary');
    let res = assert.commandWorked(coll.getDB('test').runReadCommand(
        {find: 'user', filter: {dummy: {'$exists': false}}, readConcern: {level: 'local'}}));

    assert.eq(2, res.cursor.firstBatch.length, tojson(res));

    joinUpdate();

    st.stop();
})();
