// Test that queries with a sort on text metadata return results in the correct order in a sharded
// collection.

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

let st = new ShardingTest({shards: 2});
st.stopBalancer();
let mongos = st.s0;
let shards = [st.shard0, st.shard1];
let coll = mongos.getCollection("foo.bar");
let admin = mongos.getDB("admin");
let cursor;

//
// Pre-split collection: shard 0 takes {_id: {$lt: 0}}, shard 1 takes {_id: {$gte: 0}}.
//
assert.commandWorked(admin.runCommand({enableSharding: coll.getDB().getName(), primaryShard: st.shard0.shardName}));
assert.commandWorked(admin.runCommand({shardCollection: coll.getFullName(), key: {_id: 1}}));
assert.commandWorked(admin.runCommand({split: coll.getFullName(), middle: {_id: 0}}));
assert.commandWorked(admin.runCommand({moveChunk: coll.getFullName(), find: {_id: 0}, to: st.shard1.shardName}));

//
// Insert documents into collection and create text index.
//
assert.commandWorked(coll.insert({_id: 1, a: "pizza"}));
assert.commandWorked(coll.insert({_id: -1, a: "pizza pizza"}));
assert.commandWorked(coll.insert({_id: 2, a: "pizza pizza pizza"}));
assert.commandWorked(coll.insert({_id: -2, a: "pizza pizza pizza pizza"}));
assert.commandWorked(coll.createIndex({a: "text"}));

//
// Execute query with sort on document score, verify results are in correct order.
//
let results = coll
    .find({$text: {$search: "pizza"}}, {s: {$meta: "textScore"}})
    .sort({s: {$meta: "textScore"}})
    .toArray();
assert.eq(results.length, 4, results);
assert.eq(results[0]._id, -2, results);
assert.eq(results[1]._id, 2, results);
assert.eq(results[2]._id, -1, results);
assert.eq(results[3]._id, 1, results);

// Projection not specified at all.
results = coll
    .find({$text: {$search: "pizza"}})
    .sort({s: {$meta: "textScore"}})
    .toArray();
assert.eq(results, [
    {_id: -2, a: "pizza pizza pizza pizza"},
    {_id: 2, a: "pizza pizza pizza"},
    {_id: -1, a: "pizza pizza"},
    {_id: 1, a: "pizza"},
]);

// Projection and sort specified with different field names.
results = coll
    .find({$text: {$search: "pizza"}}, {t: {$meta: "textScore"}})
    .sort({s: {$meta: "textScore"}})
    .toArray();
assert.eq(results, [
    {_id: -2, a: "pizza pizza pizza pizza", t: 1.875},
    {_id: 2, a: "pizza pizza pizza", t: 1.75},
    {_id: -1, a: "pizza pizza", t: 1.5},
    {_id: 1, a: "pizza", t: 1.1},
]);

// $meta-sort on the same field name that is included in the projection without the $meta operator.
results = coll
    .find({$text: {$search: "pizza"}}, {s: 1})
    .sort({s: {$meta: "textScore"}})
    .toArray();
assert.eq(results, [{_id: -2}, {_id: 2}, {_id: -1}, {_id: 1}]);

results = coll
    .find({$text: {$search: "pizza"}}, {s: -1})
    .sort({s: {$meta: "textScore"}})
    .toArray();
assert.eq(results, [{_id: -2}, {_id: 2}, {_id: -1}, {_id: 1}]);

if (FeatureFlagUtil.isPresentAndEnabled(mongos, "RankFusionFull")) {
    //
    // Use $score rather than $textScore when requesting the metadata for the sort (find).
    //

    results = coll
        .find({$text: {$search: "pizza"}})
        .sort({s: {$meta: "score"}})
        .toArray();
    assert.eq(results, [
        {_id: -2, a: "pizza pizza pizza pizza"},
        {_id: 2, a: "pizza pizza pizza"},
        {_id: -1, a: "pizza pizza"},
        {_id: 1, a: "pizza"},
    ]);

    //
    // Use $score rather than $textScore when requesting the metadata for the sort (aggregate).
    //

    const aggResults = coll
        .aggregate([{$match: {$text: {$search: "pizza"}}}, {$sort: {s: {$meta: "score"}, b: 1}}])
        .toArray();
    assert.eq(results, aggResults);
}

//
// Execute query with a compound sort that includes the text score along with a multikey field.
//

coll.drop();
assert.commandWorked(coll.insert({_id: 0, a: "pizza", b: [1, 4]}));
assert.commandWorked(coll.insert({_id: 1, a: "pizza pizza", b: [6, 7]}));
assert.commandWorked(coll.insert({_id: 2, a: "pizza", b: [2, 3]}));
assert.commandWorked(coll.insert({_id: 3, a: "pizza pizza", b: [5, 8]}));
assert.commandWorked(coll.createIndex({a: "text"}));

results = coll
    .find({$text: {$search: "pizza"}}, {s: {$meta: "textScore"}})
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

let aggResults = coll
    .aggregate([
        {$match: {$text: {$search: "pizza"}}},
        {$addFields: {s: {$meta: "textScore"}}},
        {$sort: {s: {$meta: "textScore"}, b: 1}},
    ])
    .toArray();
assert.eq(results, aggResults);

st.stop();
