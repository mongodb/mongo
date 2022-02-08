/**
 * This file tests the top-chunk optimization logic in splitChunk command. Whenever a chunk is
 * split, the shouldMigrate field should be set if the extreme chunk  has only a single document,
 * where extreme chunk is defined as the chunk containing either the upper or lower bound of the
 * entire shard key space.
 *
 * This test mimics the existing clustered_top_chunk_split.js but on a clustered collection.
 *
 * TODO SERVER-61557: evaluate usefulness of this test.
 *
 * @tags: [
 *   requires_fcv_53,
 * ]
 */
(function() {
'use strict';

var st = new ShardingTest({shards: 1});
var testDB = st.s.getDB('test');
assert.commandWorked(testDB.adminCommand({enableSharding: 'test'}));

var callSplit = function(db, minKey, maxKey, splitPoints) {
    jsTestLog(`callSplit minKey ${tojson(minKey)}, ${tojson(maxKey)}, ${tojson(splitPoints)}`);
    var res = st.s.adminCommand({getShardVersion: "test.user"});
    assert.commandWorked(res);
    var shardVersion = [res.version, res.versionEpoch];
    return db.runCommand({
        splitChunk: 'test.user',
        from: st.shard0.shardName,
        min: minKey,
        max: maxKey,
        keyPattern: {_id: 1},
        splitKeys: splitPoints,
        epoch: res.versionEpoch,
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
        var res = callSplit(db, {_id: MinKey}, {_id: 0}, [{_id: -2}, {_id: -1}]);
        assert.commandWorked(res);
        assert.neq(res.shouldMigrate, null, tojson(res));
        assert(bsonWoCompare(res.shouldMigrate.min, {_id: MinKey}) == 0,
               tojson(res.shouldMigrate.min));
        assert(bsonWoCompare(res.shouldMigrate.max, {_id: -2}) == 0, tojson(res.shouldMigrate.max));
    },

    // One chunk has single doc, extreme doesn't.
    //
    // Expected doc counts for new chunks:
    // [ MinKey, -1 ): 2
    // [ -1, 0): 1
    //
    function(db) {
        var res = callSplit(db, {_id: MinKey}, {_id: 0}, [{_id: -1}]);
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
        var res = callSplit(db, {_id: MinKey}, {_id: 0}, [{_id: -2}]);
        assert.commandWorked(res);
        assert.neq(res.shouldMigrate, null, tojson(res));
        assert(bsonWoCompare(res.shouldMigrate.min, {_id: MinKey}) == 0,
               tojson(res.shouldMigrate.min));
        assert(bsonWoCompare(res.shouldMigrate.max, {_id: -2}) == 0, tojson(res.shouldMigrate.max));
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
        var res = callSplit(db, {_id: 0}, {_id: MaxKey}, [{_id: 1}, {_id: 2}]);
        assert.commandWorked(res);
        assert.neq(res.shouldMigrate, null, tojson(res));
        assert(bsonWoCompare(res.shouldMigrate.min, {_id: 2}) == 0, tojson(res.shouldMigrate.min));
        assert(bsonWoCompare(res.shouldMigrate.max, {_id: MaxKey}) == 0,
               tojson(res.shouldMigrate.max));
    },

    // One chunk has single doc, extreme doesn't.
    //
    // Expected doc counts for new chunks:
    // [ 0, 1 ): 1
    // [ 1, MaxKey): 2
    //
    function(db) {
        var res = callSplit(db, {_id: 0}, {_id: MaxKey}, [{_id: 1}]);
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
        var res = callSplit(db, {_id: 0}, {_id: MaxKey}, [{_id: 2}]);
        assert.commandWorked(res);
        assert.neq(res.shouldMigrate, null, tojson(res));
        assert(bsonWoCompare(res.shouldMigrate.min, {_id: 2}) == 0, tojson(res.shouldMigrate.min));
        assert(bsonWoCompare(res.shouldMigrate.max, {_id: MaxKey}) == 0,
               tojson(res.shouldMigrate.max));
    },
];

tests.forEach(function(test) {
    // setup
    assert.commandWorked(
        testDB.createCollection("user", {clusteredIndex: {key: {_id: 1}, unique: true}}));

    assert.commandWorked(testDB.adminCommand({shardCollection: 'test.user', key: {_id: 1}}));
    assert.commandWorked(testDB.adminCommand({split: 'test.user', middle: {_id: 0}}));

    for (var _id = -3; _id < 3; _id++) {
        testDB.user.insert({_id: _id});
    }

    // run test
    test(st.rs0.getPrimary().getDB('admin'));

    // teardown
    testDB.user.drop();
});

st.stop();
})();
