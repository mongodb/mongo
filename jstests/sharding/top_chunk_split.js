/**
 * This file tests the top-chunk optimization logic in splitChunk command. Whenever a chunk is
 * split, the shouldMigrate field should be set if the extreme chunk  has only a single document,
 * where extreme chunk is defined as the chunk containing either the upper or lower bound of the
 * entire shard key space.
 */
(function() {
    'use strict';

    var st = new ShardingTest({shards: 1});

    var testDB = st.s.getDB('test');
    assert.commandWorked(testDB.adminCommand({enableSharding: 'test'}));

    var callSplit = function(db, minKey, maxKey, splitPoints) {
        var res = st.s.adminCommand({getShardVersion: "test.user"});
        assert.commandWorked(res);
        var shardVersion = [res.version, res.versionEpoch];
        return db.runCommand({
            splitChunk: 'test.user',
            from: 'shard0000',
            min: minKey,
            max: maxKey,
            keyPattern: {x: 1},
            splitKeys: splitPoints,
            shardVersion: shardVersion,
        });
    };

    var tests = [
        //
        // Lower extreme chunk tests.
        //

        // All chunks have 1 doc.
        //
        // Expected doc counts for new chunks:
        // [ MinKey, -2 ): 1
        // [ -2, -1 ): 1
        // [ -1, 0): 1
        //
        function(db) {
            var res = callSplit(db, {x: MinKey}, {x: 0}, [{x: -2}, {x: -1}]);
            assert.commandWorked(res);
            assert.neq(res.shouldMigrate, null, tojson(res));
            assert(bsonWoCompare(res.shouldMigrate.min, {x: MinKey}) == 0,
                   tojson(res.shouldMigrate.min));
            assert(bsonWoCompare(res.shouldMigrate.max, {x: -2}) == 0,
                   tojson(res.shouldMigrate.max));
        },

        // One chunk has single doc, extreme doesn't.
        //
        // Expected doc counts for new chunks:
        // [ MinKey, -1 ): 2
        // [ -1, 0): 1
        //
        function(db) {
            var res = callSplit(db, {x: MinKey}, {x: 0}, [{x: -1}]);
            assert.commandWorked(res);
            assert.eq(res.shouldMigrate, null, tojson(res));
        },

        // Only extreme has single doc.
        //
        // Expected doc counts for new chunks:
        // [ MinKey, -2 ): 1
        // [ -2, 0): 2
        //
        function(db) {
            var res = callSplit(db, {x: MinKey}, {x: 0}, [{x: -2}]);
            assert.commandWorked(res);
            assert.neq(res.shouldMigrate, null, tojson(res));
            assert(bsonWoCompare(res.shouldMigrate.min, {x: MinKey}) == 0,
                   tojson(res.shouldMigrate.min));
            assert(bsonWoCompare(res.shouldMigrate.max, {x: -2}) == 0,
                   tojson(res.shouldMigrate.max));
        },

        //
        // Upper extreme chunk tests.
        //

        // All chunks have 1 doc.
        //
        // Expected doc counts for new chunks:
        // [ 0, 1 ): 1
        // [ 1, 2 ): 1
        // [ 2, MaxKey): 1
        //
        function(db) {
            var res = callSplit(db, {x: 0}, {x: MaxKey}, [{x: 1}, {x: 2}]);
            assert.commandWorked(res);
            assert.neq(res.shouldMigrate, null, tojson(res));
            assert(bsonWoCompare(res.shouldMigrate.min, {x: 2}) == 0,
                   tojson(res.shouldMigrate.min));
            assert(bsonWoCompare(res.shouldMigrate.max, {x: MaxKey}) == 0,
                   tojson(res.shouldMigrate.max));
        },

        // One chunk has single doc, extreme doesn't.
        //
        // Expected doc counts for new chunks:
        // [ 0, 1 ): 1
        // [ 1, MaxKey): 2
        //
        function(db) {
            var res = callSplit(db, {x: 0}, {x: MaxKey}, [{x: 1}]);
            assert.commandWorked(res);
            assert.eq(res.shouldMigrate, null, tojson(res));
        },

        // Only extreme has single doc.
        //
        // Expected doc counts for new chunks:
        // [ 0, 2 ): 2
        // [ 2, MaxKey): 1
        //
        function(db) {
            var res = callSplit(db, {x: 0}, {x: MaxKey}, [{x: 2}]);
            assert.commandWorked(res);
            assert.neq(res.shouldMigrate, null, tojson(res));
            assert(bsonWoCompare(res.shouldMigrate.min, {x: 2}) == 0,
                   tojson(res.shouldMigrate.min));
            assert(bsonWoCompare(res.shouldMigrate.max, {x: MaxKey}) == 0,
                   tojson(res.shouldMigrate.max));
        },
    ];

    tests.forEach(function(test) {
        // setup
        testDB.adminCommand({shardCollection: 'test.user', key: {x: 1}});
        testDB.adminCommand({split: 'test.user', middle: {x: 0}});

        for (var x = -3; x < 3; x++) {
            testDB.user.insert({x: x});
        }

        // run test
        test(st.d0.getDB('admin'));

        // teardown
        testDB.user.drop();
    });

    st.stop();

})();
