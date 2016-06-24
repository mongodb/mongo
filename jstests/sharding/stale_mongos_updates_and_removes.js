/**
 * Tests correctness of single and multi updates and removes sent by a *stale* mongos in the
 * absence of concurrent writes or migrations.
 *
 * Single updates and removes are always targeted and versioned, because they can be retried
 * without causing the operation to be repeated on another shard (only one shard can be originally
 * targeted for a single update or remove).
 *
 * Multi updates and removes containing an equality match on the shard key are also targeted and
 * versioned, because only one shard can be originally targeted for a point query on the shard key.
 *
 * All other multi updates and removes are sent to all shards and unversioned.
 */

// Create a new sharded collection with numDocs documents, with two docs sharing each shard key
// (used for testing *multi* removes to a *specific* shard key).
var resetCollection = function() {
    assert(staleMongos.getCollection(collNS).drop());
    st.ensurePrimaryShard(dbName, st.shard0.shardName);
    assert.commandWorked(staleMongos.adminCommand({shardCollection: collNS, key: {x: 1}}));
    for (var i = 0; i < numShardKeys; i++) {
        assert.writeOK(staleMongos.getCollection(collNS).insert({x: i, fieldToUpdate: 0}));
        assert.writeOK(staleMongos.getCollection(collNS).insert({x: i, fieldToUpdate: 0}));
    }

    // Make sure data has replicated to all config servers so freshMongos finds a sharded
    // collection: freshMongos has an older optime and won't wait to see what staleMongos did
    // (shardCollection).
    st.configRS.awaitReplication();
};

// Create a new sharded collection, and split data into two chunks on different shards using the
// stale mongos. Then use the fresh mongos to consolidate the chunks onto one of the shards.
// In the end:
// staleMongos will see:
// shard0: (-inf, splitPoint]
// shard1: (splitPoint, inf]
// freshMongos will see:
// shard0: (-inf, splitPoint], (splitPoint, inf]
// shard1:
var makeStaleMongosTargetMultipleShards = function() {
    resetCollection();

    // Make sure staleMongos sees all data on first shard.
    var chunk =
        staleMongos.getCollection("config.chunks").findOne({min: {x: MinKey}, max: {x: MaxKey}});
    assert(chunk.shard === st.shard0.shardName);

    // Make sure staleMongos sees two chunks on two different shards.
    assert.commandWorked(staleMongos.adminCommand({split: collNS, middle: {x: splitPoint}}));
    assert.commandWorked(staleMongos.adminCommand(
        {moveChunk: collNS, find: {x: 0}, to: st.shard1.shardName, _waitForDelete: true}));

    // Use freshMongos to consolidate the chunks on one shard.
    assert.commandWorked(freshMongos.adminCommand(
        {moveChunk: collNS, find: {x: 0}, to: st.shard0.shardName, _waitForDelete: true}));
};

// Create a new sharded collection and move a chunk from one shard to another. In the end,
// staleMongos will see:
// shard0: (-inf, inf]
// shard1:
// freshMongos will see:
// shard0:
// shard1: (-inf, inf]
var makeStaleMongosTargetSingleShard = function() {
    resetCollection();
    // Make sure staleMongos sees all data on first shard.
    var chunk =
        staleMongos.getCollection("config.chunks").findOne({min: {x: MinKey}, max: {x: MaxKey}});
    assert(chunk.shard === st.shard0.shardName);

    // Use freshMongos to move chunk to another shard.
    assert.commandWorked(freshMongos.adminCommand(
        {moveChunk: collNS, find: {x: 0}, to: st.shard1.shardName, _waitForDelete: true}));
};

var checkAllRemoveQueries = function(makeMongosStaleFunc) {
    var multi = {justOne: false};
    var single = {justOne: true};

    var doRemove = function(query, multiOption, makeMongosStaleFunc) {
        makeMongosStaleFunc();
        assert.writeOK(staleMongos.getCollection(collNS).remove(query, multiOption));
        if (multiOption.justOne) {
            // A total of one document should have been removed from the collection.
            assert.eq(numDocs - 1, staleMongos.getCollection(collNS).find().itcount());
        } else {
            // All documents matching the query should have been removed.
            assert.eq(0, staleMongos.getCollection(collNS).find(query).itcount());
        }
    };

    var checkRemoveIsInvalid = function(query, multiOption, makeMongosStaleFunc) {
        makeMongosStaleFunc();
        var res = staleMongos.getCollection(collNS).remove(query, multiOption);
        assert.writeError(res);
    };

    // Not possible because single remove requires equality match on shard key.
    checkRemoveIsInvalid(emptyQuery, single, makeMongosStaleFunc);
    doRemove(emptyQuery, multi, makeMongosStaleFunc);

    doRemove(pointQuery, single, makeMongosStaleFunc);
    doRemove(pointQuery, multi, makeMongosStaleFunc);

    // Not possible because can't do range query on a single remove.
    checkRemoveIsInvalid(rangeQuery, single, makeMongosStaleFunc);
    doRemove(rangeQuery, multi, makeMongosStaleFunc);

    // Not possible because single remove must contain _id or shard key at top level
    // (not within $or).
    checkRemoveIsInvalid(multiPointQuery, single, makeMongosStaleFunc);
    doRemove(multiPointQuery, multi, makeMongosStaleFunc);
};

var checkAllUpdateQueries = function(makeMongosStaleFunc) {
    var oUpdate = {$inc: {fieldToUpdate: 1}};  // op-style update (non-idempotent)
    var rUpdate = {x: 0, fieldToUpdate: 1};    // replacement-style update (idempotent)
    var queryAfterUpdate = {fieldToUpdate: 1};

    var multi = {multi: true};
    var single = {multi: false};

    var doUpdate = function(query, update, multiOption, makeMongosStaleFunc) {
        makeMongosStaleFunc();
        assert.writeOK(staleMongos.getCollection(collNS).update(query, update, multiOption));
        if (multiOption.multi) {
            // All documents matching the query should have been updated.
            assert.eq(staleMongos.getCollection(collNS).find(query).itcount(),
                      staleMongos.getCollection(collNS).find(queryAfterUpdate).itcount());
        } else {
            // A total of one document should have been updated.
            assert.eq(1, staleMongos.getCollection(collNS).find(queryAfterUpdate).itcount());
        }
    };

    var checkUpdateIsInvalid = function(query, update, multiOption, makeMongosStaleFunc, err) {
        makeMongosStaleFunc();
        var res = staleMongos.getCollection(collNS).update(query, update, multiOption);
        assert.writeError(res);
    };

    // This update has inconsistent behavior as explained in SERVER-22895.
    // doUpdate(emptyQuery, rUpdate, single, makeMongosStaleFunc);
    // Not possible because replacement-style requires equality match on shard key.
    checkUpdateIsInvalid(emptyQuery, rUpdate, multi, makeMongosStaleFunc);
    // Not possible because op-style requires equality match on shard key if single update.
    checkUpdateIsInvalid(emptyQuery, oUpdate, single, makeMongosStaleFunc);
    doUpdate(emptyQuery, oUpdate, multi, makeMongosStaleFunc);

    doUpdate(pointQuery, rUpdate, single, makeMongosStaleFunc);
    // Not possible because replacement-style requires multi=false.
    checkUpdateIsInvalid(pointQuery, rUpdate, multi, makeMongosStaleFunc);
    doUpdate(pointQuery, oUpdate, single, makeMongosStaleFunc);
    doUpdate(pointQuery, oUpdate, multi, makeMongosStaleFunc);

    doUpdate(rangeQuery, rUpdate, single, makeMongosStaleFunc);
    // Not possible because replacement-style requires multi=false.
    checkUpdateIsInvalid(rangeQuery, rUpdate, multi, makeMongosStaleFunc);
    // Not possible because can't do range query on a single update.
    checkUpdateIsInvalid(rangeQuery, oUpdate, single, makeMongosStaleFunc);
    doUpdate(rangeQuery, oUpdate, multi, makeMongosStaleFunc);

    doUpdate(multiPointQuery, rUpdate, single, makeMongosStaleFunc);
    // Not possible because replacement-style requires multi=false.
    checkUpdateIsInvalid(multiPointQuery, rUpdate, multi, makeMongosStaleFunc);
    // Not possible because single remove must contain _id or shard key at top level
    // (not within $or).
    checkUpdateIsInvalid(multiPointQuery, oUpdate, single, makeMongosStaleFunc);
    doUpdate(multiPointQuery, oUpdate, multi, makeMongosStaleFunc);
};

var st = new ShardingTest({shards: 2, mongos: 2});
st.disableAutoSplit();
var dbName = 'test';
var collNS = dbName + '.foo';
var numShardKeys = 10;
var numDocs = numShardKeys * 2;
var splitPoint = numShardKeys / 2;

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
assert.commandWorked(st.s.adminCommand({shardCollection: collNS, key: {x: 1}}));

var freshMongos = st.s0;
var staleMongos = st.s1;

var emptyQuery = {};
var pointQuery = {x: 0};

// Choose a range that would fall on only one shard.
// Use (splitPoint - 1) because of SERVER-20768.
var rangeQuery = {x: {$gte: 0, $lt: splitPoint - 1}};

// Choose points that would fall on two different shards.
var multiPointQuery = {$or: [{x: 0}, {x: numShardKeys}]};

checkAllRemoveQueries(makeStaleMongosTargetSingleShard);
checkAllRemoveQueries(makeStaleMongosTargetMultipleShards);

checkAllUpdateQueries(makeStaleMongosTargetSingleShard);
checkAllUpdateQueries(makeStaleMongosTargetMultipleShards);

st.stop();
