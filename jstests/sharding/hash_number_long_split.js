// Hash sharding on a non empty collection should should not allow non-long type split keys.

(function() {
    'use strict';

    var st = new ShardingTest({shards: 2, chunkSize: 1});

    var testDB = st.s.getDB('test');
    assert.commandWorked(testDB.adminCommand({enableSharding: 'test'}));
    st.ensurePrimaryShard('test', 'shard0001');
    assert.commandWorked(testDB.adminCommand({shardCollection: 'test.user', key: {x: 'hashed'}}));

    var configDB = st.s.getDB('config');
    var chunkCountBefore = configDB.chunks.count();
    assert.gt(chunkCountBefore, 1);

    for (var x = 0; x < 100; x++) {
        testDB.user.insert({x: x});
    }

    // double-precision floating-point values
    var doubles = [47.21230129, 1.0, 0.0, -0.001];

    // integer values
    var ints = [NumberInt(-1), NumberInt(0), NumberInt(1), NumberInt(42)];

    // long values
    var longs =
        [NumberLong(5), NumberLong(-2147483647), NumberLong(2147483647), NumberLong(2147483648)];

    for (var i = 0; i < 4; i++) {
        // Split on 'middle' should fail on double value keys.
        var cmdRes = testDB.adminCommand({split: 'test.user', middle: {x: doubles[i]}});
        assert(!cmdRes.ok, 'split on middle double succeeded on {x: ' + doubles[i] + '}');

        // Split on 'middle' should fail on int value keys.
        var cmdRes = testDB.adminCommand({split: 'test.user', middle: {x: ints[i]}});
        assert(!cmdRes.ok, 'split on middle int succeeded on {x: ' + ints[i] + '}');

        // Split on 'middle' should succeed on long value keys.
        var cmdRes = testDB.adminCommand({split: 'test.user', middle: {x: longs[i]}});
        assert(cmdRes.ok, 'split on middle long failed on {x: ' + longs[i] + '}');
    }

    st.stop();
})();
