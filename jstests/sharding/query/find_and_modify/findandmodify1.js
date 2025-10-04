import {ShardingTest} from "jstests/libs/shardingtest.js";
import {findChunksUtil} from "jstests/sharding/libs/find_chunks_util.js";

let s = new ShardingTest({shards: 2});

// Make sure that findAndModify with upsert against a non-existent database and collection will
// implicitly create them both
assert.eq(
    undefined,
    assert.commandWorked(s.s0.adminCommand({listDatabases: 1, nameOnly: 1})).databases.find((dbInfo) => {
        return dbInfo.name === "NewUnshardedDB";
    }),
);

let newlyCreatedDb = s.getDB("NewUnshardedDB");
assert.eq(0, newlyCreatedDb.unsharded_coll.find({}).itcount());
newlyCreatedDb.unsharded_coll.findAndModify({query: {_id: 1}, update: {$set: {Value: "Value"}}, upsert: true});
assert.eq(1, newlyCreatedDb.unsharded_coll.find({}).itcount());

// Tests with sharded database
assert.commandWorked(s.s0.adminCommand({enablesharding: "test", primaryShard: s.shard1.shardName}));
assert.commandWorked(s.s0.adminCommand({shardcollection: "test.sharded_coll", key: {_id: 1}}));

var db = s.getDB("test");

let numObjs = 20;

// Pre-split the collection
assert.commandWorked(s.s0.adminCommand({split: "test.sharded_coll", middle: {_id: numObjs / 2}}));
assert.commandWorked(
    s.s0.adminCommand({movechunk: "test.sharded_coll", find: {_id: numObjs / 2}, to: s.shard0.shardName}),
);

let bulk = db.sharded_coll.initializeUnorderedBulkOp();
for (var i = 0; i < numObjs; i++) {
    bulk.insert({_id: i});
}
assert.commandWorked(bulk.execute());

// Put two docs in each chunk (avoid the split in 0, since there are no docs less than 0)
for (var i = 2; i < numObjs; i += 2) {
    if (i == numObjs / 2) continue;

    assert.commandWorked(s.s0.adminCommand({split: "test.sharded_coll", middle: {_id: i}}));
}

s.printChunks();
assert.eq(numObjs / 2, findChunksUtil.countChunksForNs(s.config, "test.sharded_coll"), "Split was incorrect");
assert.eq(numObjs / 4, findChunksUtil.countChunksForNs(s.config, "test.sharded_coll", {shard: s.shard0.shardName}));
assert.eq(numObjs / 4, findChunksUtil.countChunksForNs(s.config, "test.sharded_coll", {shard: s.shard1.shardName}));

// update
for (var i = 0; i < numObjs; i++) {
    assert.eq(db.sharded_coll.count({b: 1}), i, "2 A");

    var out = db.sharded_coll.findAndModify({query: {_id: i, b: null}, update: {$set: {b: 1}}});
    assert.eq(out._id, i, "2 E");

    assert.eq(db.sharded_coll.count({b: 1}), i + 1, "2 B");
}

// remove
for (var i = 0; i < numObjs; i++) {
    assert.eq(db.sharded_coll.count(), numObjs - i, "3 A");
    assert.eq(db.sharded_coll.count({_id: i}), 1, "3 B");

    var out = db.sharded_coll.findAndModify({remove: true, query: {_id: i}});

    assert.eq(db.sharded_coll.count(), numObjs - i - 1, "3 C");
    assert.eq(db.sharded_coll.count({_id: i}), 0, "3 D");
    assert.eq(out._id, i, "3 E");
}

s.stop();
