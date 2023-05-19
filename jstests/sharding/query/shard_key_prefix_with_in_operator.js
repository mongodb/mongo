// Test query on shard key prefix with $in operator containing Regex.

(function() {
"use strict";

const shardingTest = new ShardingTest({shards: 1});
const db = shardingTest.getDB("test");
const coll = db.shard_key_prefix_with_in_operator;

const shardKey = {
    a: 1,
    b: 1
};
assert.commandWorked(coll.createIndex(shardKey));

assert.commandWorked(db.adminCommand({enableSharding: db.getName()}));
assert.commandWorked(db.adminCommand({shardCollection: coll.getFullName(), key: shardKey}));

assert.doesNotThrow(() => coll.find({a: {$in: [/myRegex/, 1]}}).toArray());

shardingTest.stop();
}());
