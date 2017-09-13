/**
 * Tests that the mongod chunk filtering stage properly filters out unowned documents even after
 * the shards are restarted.
 *
 * This test involves restarting a standalone shard, so cannot be run on ephemeral storage engines.
 * A restarted standalone will lose all data when using an ephemeral storage engine.
 * @tags: [requires_persistence]
 */
(function() {
    "use strict";

    var st = new ShardingTest({shards: 2});

    var testDB = st.s.getDB('test');

    assert.commandWorked(testDB.adminCommand({enableSharding: 'test'}));
    st.ensurePrimaryShard('test', 'shard0000');
    assert.commandWorked(testDB.adminCommand({shardCollection: 'test.foo', key: {x: 1}}));

    var inserts = [];
    for (var i = 0; i < 100; i++) {
        inserts.push({x: i});
    }
    assert.writeOK(testDB.foo.insert(inserts));

    assert.commandWorked(testDB.adminCommand({split: 'test.foo', find: {x: 50}}));
    assert.commandWorked(
        testDB.adminCommand({moveChunk: 'test.foo', find: {x: 100}, to: 'shard0001'}));

    // Insert some documents directly into the shards into chunks not owned by that shard.
    st.d0.getDB('test').foo.insert({x: 100});
    st.d1.getDB('test').foo.insert({x: 0});

    st.restartMongod(0);
    st.restartMongod(1);

    var fooCount;
    for (var retries = 0; retries <= 2; retries++) {
        try {
            fooCount = testDB.foo.find().itcount();
            break;
        } catch (e) {
            // expected for reestablishing connections broken by the mongod restart.
            assert.eq(ErrorCodes.HostUnreachable, e.code, tojson(e));
        }
    }
    assert.eq(100, fooCount);

    st.stop();
}());
