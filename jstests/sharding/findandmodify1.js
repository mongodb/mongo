(function() {

    var s = new ShardingTest({name: "find_and_modify_sharded", shards: 2});

    s.adminCommand({enablesharding: "test"});
    db = s.getDB("test");
    s.ensurePrimaryShard('test', 'shard0001');
    primary = s.getPrimaryShard("test").getDB("test");
    secondary = s.getOther(primary).getDB("test");

    numObjs = 20;

    // Turn balancer off - with small numbers of chunks the balancer tries to correct all
    // imbalances, not just < 8
    s.s.getDB("config").settings.update({_id: "balancer"}, {$set: {stopped: true}}, true);

    s.adminCommand({shardcollection: "test.stuff", key: {_id: 1}});

    // pre-split the collection so to avoid interference from balancer
    s.adminCommand({split: "test.stuff", middle: {_id: numObjs / 2}});
    s.adminCommand(
        {movechunk: "test.stuff", find: {_id: numObjs / 2}, to: secondary.getMongo().name});

    var bulk = db.stuff.initializeUnorderedBulkOp();
    for (var i = 0; i < numObjs; i++) {
        bulk.insert({_id: i});
    }
    assert.writeOK(bulk.execute());

    // put two docs in each chunk (avoid the split in 0, since there are no docs less than 0)
    for (var i = 2; i < numObjs; i += 2) {
        if (i == numObjs / 2)
            continue;
        s.adminCommand({split: "test.stuff", middle: {_id: i}});
    }

    s.printChunks();
    assert.eq(numObjs / 2, s.config.chunks.count(), "split failed");
    assert.eq(numObjs / 4, s.config.chunks.count({shard: "shard0000"}));
    assert.eq(numObjs / 4, s.config.chunks.count({shard: "shard0001"}));

    // update
    for (var i = 0; i < numObjs; i++) {
        assert.eq(db.stuff.count({b: 1}), i, "2 A");

        var out = db.stuff.findAndModify({query: {_id: i, b: null}, update: {$set: {b: 1}}});
        assert.eq(out._id, i, "2 E");

        assert.eq(db.stuff.count({b: 1}), i + 1, "2 B");
    }

    // remove
    for (var i = 0; i < numObjs; i++) {
        assert.eq(db.stuff.count(), numObjs - i, "3 A");
        assert.eq(db.stuff.count({_id: i}), 1, "3 B");

        var out = db.stuff.findAndModify({remove: true, query: {_id: i}});

        assert.eq(db.stuff.count(), numObjs - i - 1, "3 C");
        assert.eq(db.stuff.count({_id: i}), 0, "3 D");
        assert.eq(out._id, i, "3 E");
    }

    s.stop();

})();
