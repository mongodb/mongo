// Tests various cases of dropping and recreating collections in the same namespace with multiple
// mongoses
(function() {
    'use strict';

    var st = new ShardingTest({shards: 3, mongos: 3});

    var config = st.s0.getDB("config");
    var admin = st.s0.getDB("admin");
    var coll = st.s0.getCollection("foo.bar");

    // Use separate mongoses for admin, inserting data, and validating results, so no single-mongos
    // tricks will work
    var staleMongos = st.s1;
    var insertMongos = st.s2;

    var shards = [];
    config.shards.find().forEach(function(doc) {
        shards.push(doc._id);
    });

    //
    // Test that inserts and queries go to the correct shard even when the collection has been
    // sharded from another mongos
    //

    jsTest.log("Enabling sharding for the first time...");

    assert.commandWorked(admin.runCommand({enableSharding: coll.getDB() + ""}));
    // TODO(PM-85): Make sure we *always* move the primary after collection lifecyle project is
    // complete
    st.ensurePrimaryShard(coll.getDB().getName(), 'shard0001');
    assert.commandWorked(admin.runCommand({shardCollection: coll + "", key: {_id: 1}}));
    st.configRS.awaitLastOpCommitted();  // TODO: Remove after collection lifecyle project (PM-85)

    var bulk = insertMongos.getCollection(coll + "").initializeUnorderedBulkOp();
    for (var i = 0; i < 100; i++) {
        bulk.insert({_id: i, test: "a"});
    }
    assert.writeOK(bulk.execute());
    assert.eq(100, staleMongos.getCollection(coll + "").find({test: "a"}).itcount());

    assert(coll.drop());

    //
    // Test that inserts and queries go to the correct shard even when the collection has been
    // resharded from another mongos, with a different key
    //

    jsTest.log("Re-enabling sharding with a different key...");

    st.ensurePrimaryShard(coll.getDB().getName(), 'shard0001');
    assert.commandWorked(coll.ensureIndex({notId: 1}));
    assert.commandWorked(admin.runCommand({shardCollection: coll + "", key: {notId: 1}}));

    bulk = insertMongos.getCollection(coll + "").initializeUnorderedBulkOp();
    for (var i = 0; i < 100; i++) {
        bulk.insert({notId: i, test: "b"});
    }
    assert.writeOK(bulk.execute());
    assert.eq(100, staleMongos.getCollection(coll + "").find({test: "b"}).itcount());
    assert.eq(0, staleMongos.getCollection(coll + "").find({test: {$in: ["a"]}}).itcount());

    assert(coll.drop());

    //
    // Test that inserts and queries go to the correct shard even when the collection has been
    // unsharded from another mongos
    //

    jsTest.log("Re-creating unsharded collection from a sharded collection...");

    bulk = insertMongos.getCollection(coll + "").initializeUnorderedBulkOp();
    for (var i = 0; i < 100; i++) {
        bulk.insert({test: "c"});
    }
    assert.writeOK(bulk.execute());

    assert.eq(100, staleMongos.getCollection(coll + "").find({test: "c"}).itcount());
    assert.eq(0, staleMongos.getCollection(coll + "").find({test: {$in: ["a", "b"]}}).itcount());

    assert(coll.drop());

    //
    // Test that inserts and queries go to correct shard even when the collection has been unsharded
    // and resharded from another mongos on a different primary
    //

    jsTest.log("Re-creating sharded collection with different primary...");

    var getOtherShard = function(shard) {
        for (var id in shards) {
            if (shards[id] != shard)
                return shards[id];
        }
    };

    assert.commandWorked(admin.runCommand({
        movePrimary: coll.getDB() + "",
        to: getOtherShard(config.databases.findOne({_id: coll.getDB() + ""}).primary)
    }));
    assert.commandWorked(admin.runCommand({shardCollection: coll + "", key: {_id: 1}}));

    bulk = insertMongos.getCollection(coll + "").initializeUnorderedBulkOp();
    for (var i = 0; i < 100; i++) {
        bulk.insert({test: "d"});
    }
    assert.writeOK(bulk.execute());

    assert.eq(100, staleMongos.getCollection(coll + "").find({test: "d"}).itcount());
    assert.eq(0,
              staleMongos.getCollection(coll + "").find({test: {$in: ["a", "b", "c"]}}).itcount());

    assert(coll.drop());

    st.stop();
})();
