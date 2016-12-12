(function() {
    'use strict';

    var s = new ShardingTest({name: "add_shard1", shards: 1, useHostname: false});

    // Create a shard and add a database; if the database is not duplicated the mongod should accept
    // it as shard
    var conn1 = MongoRunner.runMongod({'shardsvr': ""});
    var db1 = conn1.getDB("testDB");

    var numObjs = 3;
    for (var i = 0; i < numObjs; i++) {
        assert.writeOK(db1.foo.save({a: i}));
    }

    var configDB = s.s.getDB('config');
    assert.eq(null, configDB.databases.findOne({_id: 'testDB'}));

    var newShard = "myShard";
    assert.commandWorked(
        s.admin.runCommand({addshard: "localhost:" + conn1.port, name: newShard, maxSize: 1024}));

    assert.neq(null, configDB.databases.findOne({_id: 'testDB'}));

    var newShardDoc = configDB.shards.findOne({_id: newShard});
    assert.eq(1024, newShardDoc.maxSize);

    // a mongod with an existing database name should not be allowed to become a shard
    var conn2 = MongoRunner.runMongod({'shardsvr': ""});

    var db2 = conn2.getDB("otherDB");
    assert.writeOK(db2.foo.save({a: 1}));

    var db3 = conn2.getDB("testDB");
    assert.writeOK(db3.foo.save({a: 1}));

    s.config.databases.find().forEach(printjson);

    var rejectedShard = "rejectedShard";
    assert(!s.admin.runCommand({addshard: "localhost:" + conn2.port, name: rejectedShard}).ok,
           "accepted mongod with duplicate db");

    // Check that all collection that were local to the mongod's are accessible through the mongos
    var sdb1 = s.getDB("testDB");
    assert.eq(numObjs, sdb1.foo.count(), "wrong count for database that existed before addshard");

    var sdb2 = s.getDB("otherDB");
    assert.eq(0, sdb2.foo.count(), "database of rejected shard appears through mongos");

    // make sure we can move a DB from the original mongod to a previoulsy existing shard
    assert.eq(s.normalize(s.config.databases.findOne({_id: "testDB"}).primary),
              newShard,
              "DB primary is wrong");

    var origShard = s.getNonPrimaries("testDB")[0];
    s.ensurePrimaryShard("testDB", origShard);
    assert.eq(s.normalize(s.config.databases.findOne({_id: "testDB"}).primary),
              origShard,
              "DB primary didn't move");
    assert.eq(
        numObjs, sdb1.foo.count(), "wrong count after moving datbase that existed before addshard");

    // make sure we can shard the original collections
    sdb1.foo.ensureIndex({a: 1},
                         {unique: true});  // can't shard populated collection without an index
    s.adminCommand({enablesharding: "testDB"});
    s.adminCommand({shardcollection: "testDB.foo", key: {a: 1}});
    s.adminCommand({split: "testDB.foo", middle: {a: Math.floor(numObjs / 2)}});
    assert.eq(2,
              s.config.chunks.count(),
              "wrong chunk number after splitting collection that existed before");
    assert.eq(
        numObjs, sdb1.foo.count(), "wrong count after splitting collection that existed before");

    MongoRunner.stopMongod(conn1);
    MongoRunner.stopMongod(conn2);

    s.stop();

})();
