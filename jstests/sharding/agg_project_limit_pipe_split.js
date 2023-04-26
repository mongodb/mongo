// Tests that the correct number of results are returned when $limit is coalesced with $sort.
(function() {
"use strict";
load("jstests/libs/analyze_plan.js");

const shardingTest = new ShardingTest({shards: 2});
const db = shardingTest.getDB("project_limit");
const coll = db.project_limit_pipe_split;
coll.drop();
assert.commandWorked(shardingTest.s0.adminCommand({enableSharding: db.getName()}));
assert.commandWorked(
    shardingTest.s0.adminCommand({shardCollection: coll.getFullName(), key: {_id: "hashed"}}));
const bulkOp = coll.initializeOrderedBulkOp();
for (let i = 0; i < 400; ++i) {
    bulkOp.insert({x: i, y: ["a", "b", "c"], z: Math.floor(i / 12)});
}
assert.commandWorked(bulkOp.execute());

let agg = coll.aggregate([
    {$match: {$or: [{z: 9}, {z: 10}]}},
    {$sort: {x: -1}},
    {$project: {x: 1, y: 1, z: 1, _id: 0}},
    {$limit: 6},
]);
assert.eq(
    [
        {"x": 131, "y": ["a", "b", "c"], "z": 10},
        {"x": 130, "y": ["a", "b", "c"], "z": 10},
        {"x": 129, "y": ["a", "b", "c"], "z": 10},
        {"x": 128, "y": ["a", "b", "c"], "z": 10},
        {"x": 127, "y": ["a", "b", "c"], "z": 10},
        {"x": 126, "y": ["a", "b", "c"], "z": 10}
    ],
    agg.toArray());

agg = coll.aggregate(
    [{$sort: {x: 1}}, {$redact: "$$KEEP"}, {$project: {x: 1, y: 1, z: 1, _id: 0}}, {$limit: 6}]);
assert.eq(
    [
        {"x": 0, "y": ["a", "b", "c"], "z": 0},
        {"x": 1, "y": ["a", "b", "c"], "z": 0},
        {"x": 2, "y": ["a", "b", "c"], "z": 0},
        {"x": 3, "y": ["a", "b", "c"], "z": 0},
        {"x": 4, "y": ["a", "b", "c"], "z": 0},
        {"x": 5, "y": ["a", "b", "c"], "z": 0}
    ],
    agg.toArray());

agg = coll.aggregate(
    [{$sort: {x: -1}}, {$skip: 399}, {$project: {x: 1, y: 1, z: 1, _id: 0}}, {$limit: 6}]);
assert.eq([{"x": 0, "y": ["a", "b", "c"], "z": 0}], agg.toArray());

agg = coll.aggregate(
    [{$sort: {x: -1}}, {$project: {x: 1, y: 1, z: 1, _id: 0}}, {$skip: 401}, {$limit: 6}]);
assert.eq(0, agg.itcount());

agg = coll.aggregate([
    {$sort: {x: -1}},
    {$skip: 4},
    {$project: {x: 1, y: 1, z: 1, _id: 0}},
    {$skip: 3},
    {$limit: 30},
    {$skip: 3},
    {$limit: 6},
]);
assert.eq(
    [
        {"x": 389, "y": ["a", "b", "c"], "z": 32},
        {"x": 388, "y": ["a", "b", "c"], "z": 32},
        {"x": 387, "y": ["a", "b", "c"], "z": 32},
        {"x": 386, "y": ["a", "b", "c"], "z": 32},
        {"x": 385, "y": ["a", "b", "c"], "z": 32},
        {"x": 384, "y": ["a", "b", "c"], "z": 32}
    ],
    agg.toArray());

shardingTest.stop();
})();
