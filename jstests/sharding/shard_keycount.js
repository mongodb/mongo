// Tests splitting a chunk twice
(function() {
    'use strict';

    var s = new ShardingTest({name: "shard_keycount", shards: 2, mongos: 1, other: {chunkSize: 1}});

    var dbName = "test";
    var collName = "foo";
    var ns = dbName + "." + collName;

    var db = s.getDB(dbName);

    for (var i = 0; i < 10; i++) {
        db.foo.insert({_id: i});
    }

    // Enable sharding on DB
    assert.commandWorked(s.s0.adminCommand({enablesharding: dbName}));
    s.ensurePrimaryShard(dbName, 'shard0001');

    // Enable sharding on collection
    assert.commandWorked(s.s0.adminCommand({shardcollection: ns, key: {_id: 1}}));

    // Split into two chunks
    assert.commandWorked(s.s0.adminCommand({split: ns, find: {_id: 3}}));

    var coll = db.getCollection(collName);

    // Split chunk again
    assert.commandWorked(s.s0.adminCommand({split: ns, find: {_id: 3}}));

    assert.writeOK(coll.update({_id: 3}, {_id: 3}));

    // Split chunk again
    assert.commandWorked(s.s0.adminCommand({split: ns, find: {_id: 3}}));

    assert.writeOK(coll.update({_id: 3}, {_id: 3}));

    // Split chunk again
    assert.commandWorked(s.s0.adminCommand({split: ns, find: {_id: 3}}));

    s.stop();
})();
