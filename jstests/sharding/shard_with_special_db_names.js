(function() {
    'use strict';

    var s = new ShardingTest({shards: 2, mongos: 2});
    var specialDB = "[a-z]+";
    var specialNS = specialDB + ".special";

    assert.commandWorked(s.s0.adminCommand({enablesharding: "test"}));
    s.ensurePrimaryShard('test', 'shard0001');
    assert.commandWorked(s.s0.adminCommand({shardcollection: "test.data", key: {num: 1}}));

    // Test that the database will not complain "cannot have 2 database names that differs on case"
    assert.commandWorked(s.s0.adminCommand({enablesharding: specialDB}));
    s.ensurePrimaryShard(specialDB, 'shard0000');
    assert.commandWorked(s.s0.adminCommand({shardcollection: specialNS, key: {num: 1}}));

    var exists = s.getDB("config").collections.find({_id: specialNS}).itcount();
    assert.eq(exists, 1);

    // Test that drop database properly cleans up config
    s.getDB(specialDB).dropDatabase();

    var cursor = s.getDB("config").collections.find({_id: specialNS});
    assert(cursor.next()["dropped"]);
    assert(!cursor.hasNext());

    s.stop();
})();
