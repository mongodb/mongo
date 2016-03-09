// This test ensures that just attempting to read from a non-existent database or collection won't
// cause entries to be created in the catalog.
(function() {

    var shardingTest = new ShardingTest({name: 'read_does_not_create_namespaces', shards: 1});
    var db = shardingTest.getDB('NonExistentDB');

    assert.isnull(db.nonExistentColl.findOne({}));

    // Neither the database nor the collection should have been created
    assert.isnull(shardingTest.getDB('config').databases.findOne({_id: 'NonExistentDB'}));
    assert.eq(-1, shardingTest.shard0.getDBNames().indexOf('NonExistentDB'));

    shardingTest.stop();

})();
