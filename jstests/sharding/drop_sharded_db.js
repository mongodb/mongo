// Tests the dropping of a sharded database SERVER-3471 SERVER-1726
(function() {
    var st = new ShardingTest({shards: 2});

    var mongos = st.s0;
    var config = mongos.getDB("config");

    var dbA = mongos.getDB("DropSharded_A");
    var dbB = mongos.getDB("DropSharded_B");
    var dbC = mongos.getDB("DropSharded_C");

    // Dropping a database that doesn't exist will result in an info field in the response.
    var res = assert.commandWorked(dbA.dropDatabase());
    assert.eq('database does not exist', res.info);

    var numDocs = 3000;
    var numColls = 10;
    for (var i = 0; i < numDocs; i++) {
        dbA.getCollection("data" + (i % numColls)).insert({_id: i});
        dbB.getCollection("data" + (i % numColls)).insert({_id: i});
        dbC.getCollection("data" + (i % numColls)).insert({_id: i});
    }

    var key = {_id: 1};
    for (var i = 0; i < numColls; i++) {
        st.shardColl(dbA.getCollection("data" + i), key);
        st.shardColl(dbB.getCollection("data" + i), key);
        st.shardColl(dbC.getCollection("data" + i), key);
    }

    // Insert a document to an unsharded collection and make sure that the document is there.
    assert.writeOK(dbA.unsharded.insert({dummy: 1}));
    var shardName = config.databases.findOne({_id: dbA.getName()}).primary;
    var shardHostConn = new Mongo(config.shards.findOne({_id: shardName}).host);
    var dbAOnShard = shardHostConn.getDB(dbA.getName());
    assert.neq(null, dbAOnShard.unsharded.findOne({dummy: 1}));

    // Drop the non-suffixed db and ensure that it is the only one that was dropped.
    dbA.dropDatabase();
    var dbs = mongos.getDBNames();
    for (var i = 0; i < dbs.length; i++) {
        assert.neq(dbs, "" + dbA);
    }

    assert.eq(0, config.databases.count({_id: dbA.getName()}));
    assert.eq(1, config.databases.count({_id: dbB.getName()}));
    assert.eq(1, config.databases.count({_id: dbC.getName()}));

    // 10 dropped collections
    assert.eq(numColls,
              config.collections.count({_id: RegExp("^" + dbA + "\\..*"), dropped: true}));

    // 20 active (dropped is missing)
    assert.eq(numColls, config.collections.count({_id: RegExp("^" + dbB + "\\..*")}));
    assert.eq(numColls, config.collections.count({_id: RegExp("^" + dbC + "\\..*")}));

    for (var i = 0; i < numColls; i++) {
        assert.eq(numDocs / numColls, dbB.getCollection("data" + (i % numColls)).find().itcount());
        assert.eq(numDocs / numColls, dbC.getCollection("data" + (i % numColls)).find().itcount());
    }

    // Check that the unsharded collection should have been dropped.
    assert.eq(null, dbAOnShard.unsharded.findOne());

    st.stop();

})();
