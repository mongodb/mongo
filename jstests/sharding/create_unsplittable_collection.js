
/*
 * Test the test command createUnsplittableCollection. This command is a temporary wrapper on
 * shardCollection that allows you to create unsplittable collection aka tracked unsharded
 * collection. Since we use the same coordinator, we both check the createUnsplittableCollection
 * works and that shardCollection won't generate unsplittable collection
 *
 * @tags: [
 *   multiversion_incompatible,
 *   assumes_balancer_off,
 * ]
 */
(function() {
'use strict';
var kDbName = "test";

var st = new ShardingTest({shards: 2});
var mongos = st.s;

function createUnsplittableCollection_FOR_TESTING(dbName, collName) {
    return st.s.getDB(dbName).runCommand({createUnsplittableCollection: collName});
}

jsTest.log("Running test command createUnsplittableCollection to track an unsharded collection");
{
    var kColl = "first_unsharded_collection";
    var kNssUnsharded = kDbName + "." + kColl;
    assert.commandWorked(mongos.getDB("admin").runCommand({enableSharding: kDbName}));

    var result = createUnsplittableCollection_FOR_TESTING(kDbName, kColl);
    assert.commandWorked(result);

    // checking consistency
    const configDb = mongos.getDB('config');

    var unshardedColl = configDb.collections.findOne({_id: kNssUnsharded});
    assert.eq(unshardedColl._id, kNssUnsharded);
    assert.eq(unshardedColl._id, kNssUnsharded);
    assert.eq(unshardedColl.unsplittable, true);
    assert.eq(unshardedColl.key, {_id: 1});

    var configChunks = configDb.chunks.find({uuid: unshardedColl.uuid}).toArray();
    assert.eq(configChunks.length, 1);
}

jsTest.log("Check that shardCollection won't generate an unsplittable collection");
{
    var kCollSharded = "sharded_collection";
    var kNssSharded = kDbName + "." + kCollSharded;

    var result = mongos.adminCommand({shardCollection: kNssSharded, key: {_id: 1}});
    assert.commandWorked(result);

    var shardedColl = mongos.getDB('config').collections.findOne({_id: kNssSharded});
    assert.eq(shardedColl.unsplittable, undefined);
}
// cleanup
st.s.getDB(kDbName).getCollection(kColl).drop();
st.s.getDB(kDbName).getCollection(kCollSharded).drop();
st.stop();
})();
