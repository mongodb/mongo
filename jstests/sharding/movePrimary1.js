(function() {
    'use strict';

    var s = new ShardingTest({shards: 2});

    var db = s.getDB('test1');
    var c = db.foo;
    c.save({a: 1});
    c.save({a: 2});
    c.save({a: 3});
    assert.eq(3, c.count());

    assert.commandWorked(
        db.runCommand({create: "view", viewOn: "foo", pipeline: [{$match: {a: 3}}]}));

    var fromShard = s.getPrimaryShard('test1');
    var toShard = s.getOther(fromShard);

    assert.eq(3, fromShard.getDB("test1").foo.count(), "from doesn't have data before move");
    assert.eq(0, toShard.getDB("test1").foo.count(), "to has data before move");
    assert.eq(1, s.s.getDB("test1").view.count(), "count on view incorrect before move");

    s.printShardingStatus();
    assert.eq(s.normalize(s.config.databases.findOne({_id: "test1"}).primary),
              s.normalize(fromShard.name),
              "not in db correctly to start");

    var oldShardName = s.config.databases.findOne({_id: "test1"}).primary;

    assert.commandWorked(s.s0.adminCommand({movePrimary: "test1", to: toShard.name}));
    s.printShardingStatus();
    assert.eq(s.normalize(s.config.databases.findOne({_id: "test1"}).primary),
              s.normalize(toShard.name),
              "to in config db didn't change after first move");

    assert.eq(0, fromShard.getDB("test1").foo.count(), "from still has data after move");
    assert.eq(3, toShard.getDB("test1").foo.count(), "to doesn't have data after move");
    assert.eq(1, s.s.getDB("test1").view.count(), "count on view incorrect after move");

    // Move back, now using shard name instead of server address
    assert.commandWorked(s.s0.adminCommand({movePrimary: "test1", to: oldShardName}));
    s.printShardingStatus();
    assert.eq(s.normalize(s.config.databases.findOne({_id: "test1"}).primary),
              oldShardName,
              "to in config db didn't change after second move");

    assert.eq(3, fromShard.getDB("test1").foo.count(), "from doesn't have data after move back");
    assert.eq(0, toShard.getDB("test1").foo.count(), "to has data after move back");
    assert.eq(1, s.s.getDB("test1").view.count(), "count on view incorrect after move back");

    assert.commandFailedWithCode(s.s0.adminCommand({movePrimary: 'test1', to: 'dontexist'}),
                                 ErrorCodes.ShardNotFound,
                                 'attempting to use non-existent shard as primary should fail');

    s.stop();
})();
