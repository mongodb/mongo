// Test that queries with a sort on text metadata return results in the correct order in a sharded
// collection.

var st = new ShardingTest({shards: 2});
st.stopBalancer();
var mongos = st.s0;
var shards = [st.shard0, st.shard1];
var coll = mongos.getCollection("foo.bar");
var admin = mongos.getDB("admin");
var cursor;

//
// Pre-split collection: shard 0 takes {_id: {$lt: 0}}, shard 1 takes {_id: {$gte: 0}}.
//
assert.commandWorked(admin.runCommand({enableSharding: coll.getDB().getName()}));
st.ensurePrimaryShard(coll.getDB().toString(), "shard0000");
assert.commandWorked(admin.runCommand({shardCollection: coll.getFullName(), key: {_id: 1}}));
assert.commandWorked(admin.runCommand({split: coll.getFullName(), middle: {_id: 0}}));
assert.commandWorked(
    admin.runCommand({moveChunk: coll.getFullName(), find: {_id: 0}, to: "shard0001"}));

//
// Insert documents into collection and create text index.
//
assert.writeOK(coll.insert({_id: 1, a: "pizza"}));
assert.writeOK(coll.insert({_id: -1, a: "pizza pizza"}));
assert.writeOK(coll.insert({_id: 2, a: "pizza pizza pizza"}));
assert.writeOK(coll.insert({_id: -2, a: "pizza pizza pizza pizza"}));
assert.commandWorked(coll.ensureIndex({a: "text"}));

//
// Execute query with sort on document score, verify results are in correct order.
//
var results = coll.find({$text: {$search: "pizza"}}, {s: {$meta: "textScore"}})
                  .sort({s: {$meta: "textScore"}})
                  .toArray();
assert.eq(results.length, 4);
assert.eq(results[0]._id, -2);
assert.eq(results[1]._id, 2);
assert.eq(results[2]._id, -1);
assert.eq(results[3]._id, 1);

//
// Verify that mongos requires the text metadata sort to be specified in the projection.
//

// Projection not specified at all.
cursor = coll.find({$text: {$search: "pizza"}}).sort({s: {$meta: "textScore"}});
assert.throws(function() {
    cursor.next();
});

// Projection specified with incorrect field name.
cursor = coll.find({$text: {$search: "pizza"}}, {t: {$meta: "textScore"}}).sort({
    s: {$meta: "textScore"}
});
assert.throws(function() {
    cursor.next();
});

// Projection specified on correct field but with wrong sort.
cursor = coll.find({$text: {$search: "pizza"}}, {s: 1}).sort({s: {$meta: "textScore"}});
assert.throws(function() {
    cursor.next();
});
cursor = coll.find({$text: {$search: "pizza"}}, {s: -1}).sort({s: {$meta: "textScore"}});
assert.throws(function() {
    cursor.next();
});

// TODO Test sort on compound key.

st.stop();
