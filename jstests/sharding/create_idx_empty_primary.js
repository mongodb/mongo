/**
 * Test to make sure that the createIndex command gets sent to all shards.
 */
(function() {
    'use strict';

    var st = new ShardingTest({shards: 2});
    assert.commandWorked(st.s.adminCommand({enablesharding: 'test'}));
    st.ensurePrimaryShard('test', 'shard0001');

    var testDB = st.s.getDB('test');
    assert.commandWorked(testDB.adminCommand({shardcollection: 'test.user', key: {_id: 1}}));

    // Move only chunk out of primary shard.
    assert.commandWorked(
        testDB.adminCommand({movechunk: 'test.user', find: {_id: 0}, to: 'shard0000'}));

    assert.writeOK(testDB.user.insert({_id: 0}));

    var res = testDB.user.ensureIndex({i: 1});
    assert.commandWorked(res);

    var indexes = testDB.user.getIndexes();
    assert.eq(2, indexes.length);

    indexes = st.d0.getDB('test').user.getIndexes();
    assert.eq(2, indexes.length);

    indexes = st.d1.getDB('test').user.getIndexes();
    assert.eq(2, indexes.length);

    st.stop();

})();
