// Tests splitting a chunk twice
import {ShardingTest} from "jstests/libs/shardingtest.js";

let s = new ShardingTest({name: "shard_keycount", shards: 2, mongos: 1, other: {chunkSize: 1}});

let dbName = "test";
let collName = "foo";
let ns = dbName + "." + collName;

var db = s.getDB(dbName);

for (let i = 0; i < 10; i++) {
    db.foo.insert({_id: i});
}

// Enable sharding on collection
assert.commandWorked(s.s0.adminCommand({shardcollection: ns, key: {_id: 1}}));

// Split into two chunks
assert.commandWorked(s.s0.adminCommand({split: ns, find: {_id: 3}}));

let coll = db.getCollection(collName);

// Split chunk again
assert.commandWorked(s.s0.adminCommand({split: ns, find: {_id: 3}}));

assert.commandWorked(coll.update({_id: 3}, {_id: 3}));

// Split chunk again
assert.commandWorked(s.s0.adminCommand({split: ns, find: {_id: 3}}));

assert.commandWorked(coll.update({_id: 3}, {_id: 3}));

// Split chunk again
assert.commandWorked(s.s0.adminCommand({split: ns, find: {_id: 3}}));

s.stop();
