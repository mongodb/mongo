(function() {

    var s = new ShardingTest({name: "movePrimary1", shards: 2});

    initDB = function(name) {
        var db = s.getDB(name);
        var c = db.foo;
        c.save({a: 1});
        c.save({a: 2});
        c.save({a: 3});
        assert.eq(3, c.count());

        return s.getPrimaryShard(name);
    };

    from = initDB("test1");
    to = s.getOther(from);

    assert.eq(3, from.getDB("test1").foo.count(), "from doesn't have data before move");
    assert.eq(0, to.getDB("test1").foo.count(), "to has data before move");

    assert.eq(s.normalize(s.config.databases.findOne({_id: "test1"}).primary),
              s.normalize(from.name),
              "not in db correctly to start");
    s.printShardingStatus();
    oldShardName = s.config.databases.findOne({_id: "test1"}).primary;
    s.admin.runCommand({moveprimary: "test1", to: to.name});
    s.printShardingStatus();
    assert.eq(s.normalize(s.config.databases.findOne({_id: "test1"}).primary),
              s.normalize(to.name),
              "to in config db didn't change after first move");

    assert.eq(0, from.getDB("test1").foo.count(), "from still has data after move");
    assert.eq(3, to.getDB("test1").foo.count(), "to doesn't have data after move");

    // move back, now using shard name instead of server address
    s.admin.runCommand({moveprimary: "test1", to: oldShardName});
    s.printShardingStatus();
    assert.eq(s.normalize(s.config.databases.findOne({_id: "test1"}).primary),
              oldShardName,
              "to in config db didn't change after second move");

    assert.eq(3, from.getDB("test1").foo.count(), "from doesn't have data after move back");
    assert.eq(0, to.getDB("test1").foo.count(), "to has data after move back");

    // attempting to move primary DB to non-existent shard should error out with appropriate code
    var res = s.admin.runCommand({movePrimary: 'test1', to: 'dontexist'});
    assert.commandFailed(res, 'attempting to use non-existent shard as primary should error out');
    // ErrorCodes::ShardNotFound === 70
    assert.eq(res.code, 70, 'ShardNotFound code not used');

    s.stop();

})();
