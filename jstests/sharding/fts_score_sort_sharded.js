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
st.ensurePrimaryShard(coll.getDB().toString(), st.shard0.shardName);
assert.commandWorked(admin.runCommand({shardCollection: coll.getFullName(), key: {_id: 1}}));
assert.commandWorked(admin.runCommand({split: coll.getFullName(), middle: {_id: 0}}));
assert.commandWorked(
    admin.runCommand({moveChunk: coll.getFullName(), find: {_id: 0}, to: st.shard1.shardName}));

//
// Insert documents into collection and create text index.
//
assert.commandWorked(coll.insert({_id: 1, a: "pizza"}));
assert.commandWorked(coll.insert({_id: -1, a: "pizza pizza"}));
assert.commandWorked(coll.insert({_id: 2, a: "pizza pizza pizza"}));
assert.commandWorked(coll.insert({_id: -2, a: "pizza pizza pizza pizza"}));
assert.commandWorked(coll.ensureIndex({a: "text"}));

//
// Execute query with sort on document score, verify results are in correct order.
//
var results = coll.find({$text: {$search: "pizza"}}, {s: {$meta: "textScore"}})
                  .sort({s: {$meta: "textScore"}})
                  .toArray();
assert.eq(results.length, 4, results);
assert.eq(results[0]._id, -2, results);
assert.eq(results[1]._id, 2, results);
assert.eq(results[2]._id, -1, results);
assert.eq(results[3]._id, 1, results);

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

//
// Execute query with a compound sort that includes the text score along with a multikey field.
//

coll.drop();
assert.commandWorked(coll.insert({_id: 0, a: "pizza", b: [1, 4]}));
assert.commandWorked(coll.insert({_id: 1, a: "pizza pizza", b: [6, 7]}));
assert.commandWorked(coll.insert({_id: 2, a: "pizza", b: [2, 3]}));
assert.commandWorked(coll.insert({_id: 3, a: "pizza pizza", b: [5, 8]}));
assert.commandWorked(coll.ensureIndex({a: "text"}));

results = coll.find({$text: {$search: "pizza"}}, {s: {$meta: "textScore"}})
              .sort({s: {$meta: "textScore"}, b: 1})
              .toArray();
assert.eq(results.length, 4, results);
assert.eq(results[0]._id, 3, results);
assert.eq(results[1]._id, 1, results);
assert.eq(results[2]._id, 0, results);
assert.eq(results[3]._id, 2, results);

//
// Repeat the query with an aggregation pipeline and verify that the result is the same.
//

var aggResults = coll.aggregate([
                         {$match: {$text: {$search: "pizza"}}},
                         {$addFields: {s: {$meta: "textScore"}}},
                         {$sort: {s: {$meta: "textScore"}, b: 1}}
                     ])
                     .toArray();
assert.eq(results, aggResults);

st.stop();
