/**
 * Tests for $jsonSchema queries in a sharded cluster.
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";

const dbName = "json_schema_sharding";

let st = new ShardingTest({shards: 2, mongos: 1});

assert.commandWorked(st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.name}));

const testDB = st.s.getDB(dbName);
const coll = testDB.json_schema_sharding;
coll.drop();

// Shard the collection on _id.
assert.commandWorked(testDB.adminCommand({shardCollection: coll.getFullName(), key: {_id: 1}}));

// Split the collection into 4 chunks: [MinKey, -100), [-100, 0), [0, 100), [100, MaxKey).
assert.commandWorked(testDB.adminCommand({split: coll.getFullName(), middle: {_id: -100}}));
assert.commandWorked(testDB.adminCommand({split: coll.getFullName(), middle: {_id: 0}}));
assert.commandWorked(testDB.adminCommand({split: coll.getFullName(), middle: {_id: 100}}));

// Move the [0, 100) and [100, MaxKey) chunks to st.shard1.shardName.
assert.commandWorked(testDB.adminCommand({moveChunk: coll.getFullName(), find: {_id: 50}, to: st.shard1.shardName}));
assert.commandWorked(testDB.adminCommand({moveChunk: coll.getFullName(), find: {_id: 150}, to: st.shard1.shardName}));

// Write one document into each of the chunks.
assert.commandWorked(coll.insert({_id: -150, a: 1}));
assert.commandWorked(coll.insert({_id: -50, a: 10}));
assert.commandWorked(coll.insert({_id: 50, a: "str"}));
assert.commandWorked(coll.insert({_id: 150}));

// Test that $jsonSchema in a find command returns the correct results.
assert.eq(4, coll.find({$jsonSchema: {}}).itcount());
assert.eq(3, coll.find({$jsonSchema: {properties: {a: {type: "number"}}}}).itcount());
assert.eq(4, coll.find({$jsonSchema: {required: ["_id"]}}).itcount());
assert.eq(1, coll.find({$jsonSchema: {properties: {_id: {minimum: 150}}}}).itcount());

// Test that $jsonSchema works correctly in an update command.
let res = coll.update(
    {$jsonSchema: {properties: {_id: {type: "number", minimum: 100}, a: {type: "number"}}}},
    {$inc: {a: 1}},
    {multi: true},
);
assert.commandWorked(res);
assert.eq(1, res.nModified);

const schema = {
    properties: {_id: {type: "number", minimum: 100}},
    required: ["_id"],
};
res = coll.update({$jsonSchema: schema}, {$set: {b: 1}}, {multi: true});
assert.commandWorked(res);
assert.eq(1, res.nModified);

// Test that $jsonSchema works correctly in a findAndModify command.
res = coll.findAndModify({query: {_id: 150, $jsonSchema: schema}, update: {$set: {b: 1}}});
assert.eq(1, res.b);

st.stop();
