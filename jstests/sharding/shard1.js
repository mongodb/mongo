/**
* this tests some of the ground work
*/
(function() {
    'use strict';

    var s = new ShardingTest({shards: 2});
    var db = s.getDB("test");

    assert.writeOK(db.foo.insert({num: 1, name: "eliot"}));
    assert.writeOK(db.foo.insert({num: 2, name: "sara"}));
    assert.writeOK(db.foo.insert({num: -1, name: "joe"}));

    assert.commandWorked(db.foo.ensureIndex({num: 1}));

    assert.eq(3, db.foo.find().length(), "A");

    const shardCommand = {shardcollection: "test.foo", key: {num: 1}};

    assert.commandFailed(s.s0.adminCommand(shardCommand));

    assert.commandWorked(s.s0.adminCommand({enablesharding: "test"}));
    s.ensurePrimaryShard('test', 'shard0001');

    assert.eq(3, db.foo.find().length(), "after partitioning count failed");

    assert.commandWorked(s.s0.adminCommand(shardCommand));
    assert.commandFailed(s.s0.adminCommand({shardCollection: 'test', key: {x: 1}}));
    assert.commandFailed(s.s0.adminCommand({shardCollection: '.foo', key: {x: 1}}));

    var cconfig = s.config.collections.findOne({_id: "test.foo"});
    assert(cconfig, "No collection entry found for test.foo");

    delete cconfig.lastmod;
    delete cconfig.dropped;
    delete cconfig.lastmodEpoch;
    delete cconfig.uuid;

    assert.eq(cconfig, {_id: "test.foo", key: {num: 1}, unique: false}, "Sharded content mismatch");

    s.config.collections.find().forEach(printjson);

    assert.eq(1, s.config.chunks.count({"ns": "test.foo"}), "num chunks A");
    assert.eq(3, db.foo.find().length(), "after sharding, no split count failed");

    s.stop();
})();
