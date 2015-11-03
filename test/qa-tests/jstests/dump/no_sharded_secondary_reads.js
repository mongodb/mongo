(function(){
    // This test makes sure that mongodump does not do secondary reads when talking to a mongos.
    // We do this by creating a sharded topology against a replica set where all the nodes have 
    // profiling enabled. After running mongodump, we can query all of the profile collections
    // to see if the queries reached any of the secondary nodes (they shouldn't!). 
    //
    var NODE_COUNT = 5;
    var st = new ShardingTest({shards : {rs0 : {nodes : NODE_COUNT}}});
    var replTest = st.rs0;
    var conn = st.s;

    var db = conn.getDB("test");
    var replDB = replTest.getMaster().getDB("test");

    db.a.insert({a:1});
    db.a.insert({a:2});
    db.a.insert({a:3});
    db.a.insert({a:4});
    db.a.insert({a:5});
    db.a.insert({a:5});
    assert.eq(db.a.count(), replDB.a.count(), "count should match for mongos and mongod");

    printjson(replDB.setProfilingLevel(2));
    var secondaries = [];
    // get all the secondaries and enable profiling
    for (var i = 0; i < NODE_COUNT; i++) {
        var sDB = replTest.nodes[i].getDB("test");
        if (sDB.isMaster().secondary) {
            sDB.setProfilingLevel(2);
            secondaries.push(sDB);
        }
    }
    print("done enabling profiling");
    printjson(secondaries);

    // perform 3 queries with the shell (sanity check before using mongodump)
    assert.eq(db.a.find({a: {"$lte":3}}).toArray().length, 3);
    assert.eq(db.a.find({a:3}).toArray().length, 1);
    assert.eq(db.a.find({a:5}).toArray().length, 2);
    // assert that the shell queries happened only on primaries
    profQuery= {ns: "test.a", op:"query"};
    assert.eq(replDB.system.profile.find().count(profQuery), 3,
        "three queries should have been logged");
    for (var i = 0; i < secondaries.length; i++) {
      assert.eq(secondaries[i].system.profile.find(profQuery).count(), 0,
          "no queries should be against secondaries");
    }

    print("running mongodump on mongos");
    var mongosAddr = st.getConnNames()[0];
    runMongoProgram("mongodump", "--host", st.s.host, "-vvvv");
    assert.eq(replDB.system.profile.find(profQuery).count(), 4, "queries are routed to primary");
    assert.eq(replDB.system.profile.find({ns:"test.a", op:"query", $or:[{"query.$snapshot": true}, {"query.snapshot":true}]}).count(), 1);
    printjson(replDB.system.profile.find(profQuery).toArray());
    // make sure the secondaries saw 0 queries
    for (var i = 0; i < secondaries.length; i++) {
      print("checking secondary " + i);
      assert.eq(secondaries[i].system.profile.find(profQuery).count(), 0,
          "no dump queries should be against secondaries");
    }

}());
