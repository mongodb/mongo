//
// Upsert behavior tests for sharding
// NOTE: Generic upsert behavior tests belong in the core suite
//
(function() {
    'use strict';

    var st = new ShardingTest({shards: 2, mongos: 1});

    var mongos = st.s0;
    var admin = mongos.getDB("admin");
    var shards = mongos.getCollection("config.shards").find().toArray();
    var coll = mongos.getCollection("foo.bar");

    assert(admin.runCommand({enableSharding: coll.getDB() + ""}).ok);
    st.ensurePrimaryShard(coll.getDB().getName(), 'shard0001');

    var upsertedResult = function(query, expr) {
        coll.remove({});
        return coll.update(query, expr, {upsert: true});
    };

    var upsertedField = function(query, expr, fieldName) {
        assert.writeOK(upsertedResult(query, expr));
        return coll.findOne()[fieldName];
    };

    var upsertedId = function(query, expr) {
        return upsertedField(query, expr, "_id");
    };

    var upsertedXVal = function(query, expr) {
        return upsertedField(query, expr, "x");
    };

    st.ensurePrimaryShard(coll.getDB() + "", shards[0]._id);
    assert.commandWorked(admin.runCommand({shardCollection: coll + "", key: {x: 1}}));
    assert.commandWorked(admin.runCommand({split: coll + "", middle: {x: 0}}));
    assert.commandWorked(admin.runCommand(
        {moveChunk: coll + "", find: {x: 0}, to: shards[1]._id, _waitForDelete: true}));

    st.printShardingStatus();

    // upserted update replacement would result in no shard key
    assert.writeError(upsertedResult({x: 1}, {}));

    // updates with upsert must contain shard key in query when $op style
    assert.eq(1, upsertedXVal({x: 1}, {$set: {a: 1}}));
    assert.eq(1, upsertedXVal({x: {$eq: 1}}, {$set: {a: 1}}));
    assert.eq(1, upsertedXVal({x: {$all: [1]}}, {$set: {a: 1}}));
    assert.eq(1, upsertedXVal({x: {$in: [1]}}, {$set: {a: 1}}));
    assert.eq(1, upsertedXVal({$and: [{x: {$eq: 1}}]}, {$set: {a: 1}}));
    assert.eq(1, upsertedXVal({$or: [{x: {$eq: 1}}]}, {$set: {a: 1}}));

    // shard key not extracted
    assert.writeError(upsertedResult({}, {$set: {a: 1, x: 1}}));
    assert.writeError(upsertedResult({x: {$gt: 1}}, {$set: {a: 1, x: 1}}));

    // Shard key type errors
    assert.writeError(upsertedResult({x: undefined}, {$set: {a: 1}}));
    assert.writeError(upsertedResult({x: [1, 2]}, {$set: {a: 1}}));
    assert.writeError(upsertedResult({x: {$eq: {$gt: 5}}}, {$set: {a: 1}}));
    // Regex shard key is not extracted from queries, even exact matches
    assert.writeError(upsertedResult({x: {$eq: /abc/}}, {$set: {a: 1}}));

    // nested field extraction always fails with non-nested key - like _id, we require setting the
    // elements directly
    assert.writeError(upsertedResult({"x.x": 1}, {$set: {a: 1}}));
    assert.writeError(upsertedResult({"x.x": {$eq: 1}}, {$set: {a: 1}}));

    coll.drop();

    st.ensurePrimaryShard(coll.getDB() + "", shards[0]._id);
    assert.commandWorked(admin.runCommand({shardCollection: coll + "", key: {'x.x': 1}}));
    assert.commandWorked(admin.runCommand({split: coll + "", middle: {'x.x': 0}}));
    assert.commandWorked(admin.runCommand(
        {moveChunk: coll + "", find: {'x.x': 0}, to: shards[1]._id, _waitForDelete: true}));

    st.printShardingStatus();

    // nested field extraction with nested shard key
    assert.docEq({x: 1}, upsertedXVal({"x.x": 1}, {$set: {a: 1}}));
    assert.docEq({x: 1}, upsertedXVal({"x.x": {$eq: 1}}, {$set: {a: 1}}));
    assert.docEq({x: 1}, upsertedXVal({"x.x": {$all: [1]}}, {$set: {a: 1}}));
    assert.docEq({x: 1}, upsertedXVal({$and: [{"x.x": {$eq: 1}}]}, {$set: {a: 1}}));
    assert.docEq({x: 1}, upsertedXVal({$or: [{"x.x": {$eq: 1}}]}, {$set: {a: 1}}));

    // Can specify siblings of nested shard keys
    assert.docEq({x: 1, y: 1}, upsertedXVal({"x.x": 1, "x.y": 1}, {$set: {a: 1}}));
    assert.docEq({x: 1, y: {z: 1}}, upsertedXVal({"x.x": 1, "x.y.z": 1}, {$set: {a: 1}}));

    // No arrays at any level
    assert.writeError(upsertedResult({"x.x": []}, {$set: {a: 1}}));
    assert.writeError(upsertedResult({x: {x: []}}, {$set: {a: 1}}));
    assert.writeError(upsertedResult({x: [{x: 1}]}, {$set: {a: 1}}));

    // Can't set sub-fields of nested key
    assert.writeError(upsertedResult({"x.x.x": {$eq: 1}}, {$set: {a: 1}}));

    st.stop();

})();
