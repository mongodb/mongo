/**
 * Basic test from the drop collection command on a sharded cluster that verifies collections are
 * cleanuped up properly.
 */
(function() {
    "use strict";

    var st = new ShardingTest({shards: 2});

    var testDB = st.s.getDB('test');

    // Test dropping an unsharded collection.

    assert.writeOK(testDB.bar.insert({x: 1}));
    assert.neq(null, testDB.bar.findOne({x: 1}));

    assert.commandWorked(testDB.runCommand({drop: 'bar'}));
    assert.eq(null, testDB.bar.findOne({x: 1}));

    // Test dropping a sharded collection.

    assert.commandWorked(st.s.adminCommand({enableSharding: 'test'}));
    st.ensurePrimaryShard('test', 'shard0000');
    st.s.adminCommand({shardCollection: 'test.user', key: {_id: 1}});
    st.s.adminCommand({split: 'test.user', middle: {_id: 0}});
    assert.commandWorked(
        st.s.adminCommand({moveChunk: 'test.user', find: {_id: 0}, to: 'shard0001'}));

    assert.writeOK(testDB.user.insert({_id: 10}));
    assert.writeOK(testDB.user.insert({_id: -10}));

    assert.neq(null, st.d0.getDB('test').user.findOne({_id: -10}));
    assert.neq(null, st.d1.getDB('test').user.findOne({_id: 10}));

    var configDB = st.s.getDB('config');
    var collDoc = configDB.collections.findOne({_id: 'test.user'});
    assert(!collDoc.dropped);

    assert.eq(2, configDB.chunks.count({ns: 'test.user'}));

    assert.commandWorked(testDB.runCommand({drop: 'user'}));

    assert.eq(null, st.d0.getDB('test').user.findOne());
    assert.eq(null, st.d1.getDB('test').user.findOne());

    collDoc = configDB.collections.findOne({_id: 'test.user'});
    assert(collDoc.dropped);

    assert.eq(0, configDB.chunks.count({ns: 'test.user'}));

    st.stop();

})();
