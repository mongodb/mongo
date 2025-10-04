/**
 * Basic test of sharding with a hashed shard key and other unique index. Does 2 things and checks
 * for consistent error:
 * 1. shard collection on hashed "a", ensure unique index {a:1, b:1}
 * 2. reverse order
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";

let s = new ShardingTest({shards: 1, mongos: 1});
let dbName = "test";
let collName = "foo";
let ns = dbName + "." + collName;
var db = s.getDB(dbName);
let coll = db.getCollection(collName);

// Enable sharding on DB
assert.commandWorked(db.adminCommand({enablesharding: dbName}));

// Shard a fresh collection using a hashed shard key
assert.commandWorked(db.adminCommand({shardcollection: ns, key: {a: "hashed"}}));

// Create unique index
assert.commandWorked(coll.createIndex({a: 1, b: 1}, {unique: true}));

jsTest.log("------ indexes -------");
jsTest.log(tojson(coll.getIndexes()));

// Second Part
jsTest.log("------ dropping sharded collection to start part 2 -------");
coll.drop();

// Create unique index
assert.commandWorked(coll.createIndex({a: 1, b: 1}, {unique: true}));

// shard a fresh collection using a hashed shard key
assert.commandWorked(db.adminCommand({shardcollection: ns, key: {a: "hashed"}}), "shardcollection didn't worked 2");

s.printShardingStatus();
jsTest.log("------ indexes 2-------");
jsTest.log(tojson(coll.getIndexes()));

s.stop();
