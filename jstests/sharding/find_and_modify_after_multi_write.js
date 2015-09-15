(function() {
"use strict";

/**
 * Test that a targetted findAndModify will be properly routed after executing a write that
 * does not perform any shard version checks.
 */
var runTest = function(writeFunc) {
    var st = new ShardingTest({ shards: 2, mongos: 2 });

    var testDB = st.s.getDB('test');
    testDB.dropDatabase();

    testDB.adminCommand({ enableSharding: 'test' });
    st.ensurePrimaryShard('test', 'shard0000');
    testDB.adminCommand({ shardCollection: 'test.user', key: { x: 1 }});
    testDB.adminCommand({ split: 'test.user', middle: { x: 0 }});
    testDB.adminCommand({ moveChunk: 'test.user', find: { x: 0 }, to: 'shard0001' });

    var testDB2 = st.s1.getDB('test');
    testDB2.user.insert({ x: 123456 });

    // Move chunk to bump version on a different mongos.
    testDB.adminCommand({ moveChunk: 'test.user', find: { x: 0 }, to: 'shard0000' });

    writeFunc(testDB2);

    // Issue a targetted findAndModify and check that it was upserted to the right shard.
    var res = testDB2.runCommand({
        findAndModify: 'user',
        query: { x: 100 },
        update: { $set: { y: 1 }},
        upsert: true
    });

    assert.commandWorked(res);

    assert.neq(null, st.d0.getDB('test').user.findOne({ x: 100 }));
    assert.eq(null, st.d1.getDB('test').user.findOne({ x: 100 }));

    st.stop();
};

runTest(function(db) {
    db.user.update({}, { $inc: { y: 987654 }}, false, true);
});

runTest(function(db) {
    db.user.remove({ y: 'noMatch' }, false);
});

})();
