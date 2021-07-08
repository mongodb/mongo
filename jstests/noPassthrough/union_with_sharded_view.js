// Test that sharded $unionWith can resolve sharded views correctly when target shards are on
// different, non-primary shards.
// @tags: [requires_sharding, requires_fcv_50]
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");  // For assertArrayEq.

const sharded = new ShardingTest({mongos: 1, shards: 4, config: 1});
assert(sharded.adminCommand({enableSharding: "test"}));

const testDBName = "test";
const testDB = sharded.getDB(testDBName);

const local = testDB.local;
local.drop();
assert.commandWorked(local.createIndex({shard_key: 1}));

const foreign = testDB.foreign;
foreign.drop();
assert.commandWorked(foreign.createIndex({shard_key: 1}));

const otherForeign = testDB.otherForeign;
otherForeign.drop();
assert.commandWorked(otherForeign.createIndex({shard_key: 1}));

assert.commandWorked(local.insertMany([
    {_id: 1, shard_key: "shard1"},
    {_id: 2, shard_key: "shard1"},
    {_id: 3, shard_key: "shard1"},
]));

assert.commandWorked(foreign.insertMany([
    {_id: 4, shard_key: "shard2"},
    {_id: 5, shard_key: "shard2"},
    {_id: 6, shard_key: "shard2"},
]));

assert.commandWorked(otherForeign.insertMany([
    {_id: 7, shard_key: "shard3"},
    {_id: 8, shard_key: "shard3"},
]));

sharded.ensurePrimaryShard(testDBName, sharded.shard0.shardName);
assert(sharded.s.adminCommand({shardCollection: local.getFullName(), key: {shard_key: 1}}));
assert(sharded.s.adminCommand({shardCollection: foreign.getFullName(), key: {shard_key: 1}}));
assert(sharded.s.adminCommand({shardCollection: otherForeign.getFullName(), key: {shard_key: 1}}));

function testUnionWithView(pipeline, expected) {
    assertArrayEq({actual: local.aggregate(pipeline).toArray(), expected});
}

function checkView(viewName, expected) {
    assertArrayEq({actual: testDB[viewName].find({}).toArray(), expected});
}

// Place all of local on shard1 and all of foreign on shard2 to force
// CommandOnShardedViewNotSupportedOnMongod exceptions where a shard cannot resolve a view
// definition.
assert.commandWorked(testDB.adminCommand(
    {moveChunk: local.getFullName(), find: {shard_key: "shard1"}, to: sharded.shard1.shardName}));
assert.commandWorked(testDB.adminCommand(
    {moveChunk: foreign.getFullName(), find: {shard_key: "shard2"}, to: sharded.shard2.shardName}));
assert.commandWorked(testDB.adminCommand({
    moveChunk: otherForeign.getFullName(),
    find: {shard_key: "shard3"},
    to: sharded.shard3.shardName
}));

// Create a view on foreign with a pipeline that references a namespace that the top-level unionWith
// has not yet encountered and verify that the view can be queried correctly.
assert.commandWorked(
    testDB.createView("unionView", foreign.getName(), [{$unionWith: "otherForeign"}]));
checkView("unionView", [
    {_id: 4, shard_key: "shard2"},
    {_id: 5, shard_key: "shard2"},
    {_id: 6, shard_key: "shard2"},
    {_id: 7, shard_key: "shard3"},
    {_id: 8, shard_key: "shard3"},
]);

testUnionWithView(
    [
        {$unionWith: "unionView"},
    ],
    [
        {_id: 1, shard_key: "shard1"},
        {_id: 2, shard_key: "shard1"},
        {_id: 3, shard_key: "shard1"},
        {_id: 4, shard_key: "shard2"},
        {_id: 5, shard_key: "shard2"},
        {_id: 6, shard_key: "shard2"},
        {_id: 7, shard_key: "shard3"},
        {_id: 8, shard_key: "shard3"},
    ]);

sharded.stop();
}());
