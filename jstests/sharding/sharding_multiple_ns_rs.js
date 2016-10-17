(function() {
    load("jstests/replsets/rslib.js");

    var s = new ShardingTest(
        {name: "Sharding multiple ns", shards: 1, mongos: 1, other: {rs: true, chunkSize: 1}});

    s.adminCommand({enablesharding: "test"});
    s.adminCommand({shardcollection: "test.foo", key: {_id: 1}});

    db = s.getDB("test");

    var bulk = db.foo.initializeUnorderedBulkOp();
    var bulk2 = db.bar.initializeUnorderedBulkOp();
    for (i = 0; i < 100; i++) {
        bulk.insert({_id: i, x: i});
        bulk2.insert({_id: i, x: i});
    }
    assert.writeOK(bulk.execute());
    assert.writeOK(bulk2.execute());

    sh.splitAt("test.foo", {_id: 50});

    other = new Mongo(s.s.name);
    dbother = other.getDB("test");

    assert.eq(5, db.foo.findOne({_id: 5}).x);
    assert.eq(5, dbother.foo.findOne({_id: 5}).x);

    assert.eq(5, db.bar.findOne({_id: 5}).x);
    assert.eq(5, dbother.bar.findOne({_id: 5}).x);

    s._rs[0].test.awaitReplication();
    s._rs[0].test.stopMaster(15);

    // Wait for the primary to come back online...
    var primary = s._rs[0].test.getPrimary();

    // Wait for the mongos to recognize the new primary...
    awaitRSClientHosts(db.getMongo(), primary, {ismaster: true});

    assert.eq(5, db.foo.findOne({_id: 5}).x);
    assert.eq(5, db.bar.findOne({_id: 5}).x);

    s.adminCommand({shardcollection: "test.bar", key: {_id: 1}});
    sh.splitAt("test.bar", {_id: 50});

    yetagain = new Mongo(s.s.name);
    assert.eq(5, yetagain.getDB("test").bar.findOne({_id: 5}).x);
    assert.eq(5, yetagain.getDB("test").foo.findOne({_id: 5}).x);

    assert.eq(5, dbother.bar.findOne({_id: 5}).x);
    assert.eq(5, dbother.foo.findOne({_id: 5}).x);

    s.stop();

})();
