(function() {
    "use strict";

    /**
     * Test that a targetted findAndModify will be properly routed after executing a write that
     * does not perform any shard version checks.
     */
    var runTest = function(writeFunc) {
        var st = new ShardingTest({shards: 2, mongos: 2});

        var testDB = st.s.getDB('test');

        assert.commandWorked(testDB.adminCommand({enableSharding: 'test'}));
        st.ensurePrimaryShard('test', 'shard0000');

        assert.commandWorked(testDB.adminCommand({shardCollection: 'test.user', key: {x: 1}}));
        assert.commandWorked(testDB.adminCommand({split: 'test.user', middle: {x: 0}}));
        assert.commandWorked(testDB.adminCommand(
            {moveChunk: 'test.user', find: {x: 0}, to: 'shard0001', _waitForDelete: true}));

        var testDB2 = st.s1.getDB('test');
        testDB2.user.insert({x: 123456});

        // Move chunk to bump version on a different mongos.
        assert.commandWorked(testDB.adminCommand(
            {moveChunk: 'test.user', find: {x: 0}, to: 'shard0000', _waitForDelete: true}));

        // Issue a targetted findAndModify and check that it was upserted to the right shard.
        assert.commandWorked(testDB2.runCommand(
            {findAndModify: 'user', query: {x: 100}, update: {$set: {y: 1}}, upsert: true}));

        assert.neq(null, st.d0.getDB('test').user.findOne({x: 100}));
        assert.eq(null, st.d1.getDB('test').user.findOne({x: 100}));

        // At this point, s1 thinks the version of 'test.user' is 2, bounce it again so it gets
        // incremented to 3
        assert.commandWorked(testDB.adminCommand(
            {moveChunk: 'test.user', find: {x: 0}, to: 'shard0001', _waitForDelete: true}));

        assert.commandWorked(testDB2.runCommand(
            {findAndModify: 'user', query: {x: 200}, update: {$set: {y: 1}}, upsert: true}));

        assert.eq(null, st.d0.getDB('test').user.findOne({x: 200}));
        assert.neq(null, st.d1.getDB('test').user.findOne({x: 200}));

        // At this point, s0 thinks the version of 'test.user' is 3, bounce it again so it gets
        // incremented to 4
        assert.commandWorked(testDB.adminCommand(
            {moveChunk: 'test.user', find: {x: 0}, to: 'shard0000', _waitForDelete: true}));

        // Ensure that write commands with multi version do not reset the connection shard version
        // to
        // ignored.
        writeFunc(testDB2);

        assert.commandWorked(testDB2.runCommand(
            {findAndModify: 'user', query: {x: 300}, update: {$set: {y: 1}}, upsert: true}));

        assert.neq(null, st.d0.getDB('test').user.findOne({x: 300}));
        assert.eq(null, st.d1.getDB('test').user.findOne({x: 300}));

        st.stop();
    };

    runTest(function(db) {
        db.user.update({}, {$inc: {y: 987654}}, false, true);
    });

    runTest(function(db) {
        db.user.remove({y: 'noMatch'}, false);
    });

})();
