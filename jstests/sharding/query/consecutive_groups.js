// SERVER-6179: support for two $groups in sharded agg
// @tags: [
//   requires_sharding,
//   requires_spawning_own_processes,
// ]
import {ShardingTest} from "jstests/libs/shardingtest.js";

let s = new ShardingTest({shards: 2});

assert.commandWorked(s.s0.adminCommand({enablesharding: "test", primaryShard: s.shard1.shardName}));
assert.commandWorked(s.s0.adminCommand({shardcollection: "test.data", key: {_id: 1}}));

let d = s.getDB("test");

// Insert _id values 0 - 99
let N = 100;

let bulkOp = d.data.initializeOrderedBulkOp();
for (let i = 0; i < N; ++i) {
    bulkOp.insert({_id: i, i: i % 10});
}
bulkOp.execute();

// Split the data into 3 chunks
assert.commandWorked(s.s0.adminCommand({split: "test.data", middle: {_id: 33}}));
assert.commandWorked(s.s0.adminCommand({split: "test.data", middle: {_id: 66}}));

// Migrate the middle chunk to another shard
assert.commandWorked(
    s.s0.adminCommand({movechunk: "test.data", find: {_id: 50}, to: s.getOther(s.getPrimaryShard("test")).name}),
);

// Check that we get results rather than an error
let result = d.data
    .aggregate(
        {$group: {_id: "$_id", i: {$first: "$i"}}},
        {$group: {_id: "$i", avg_id: {$avg: "$_id"}}},
        {$sort: {_id: 1}},
    )
    .toArray();
let expected = [
    {"_id": 0, "avg_id": 45},
    {"_id": 1, "avg_id": 46},
    {"_id": 2, "avg_id": 47},
    {"_id": 3, "avg_id": 48},
    {"_id": 4, "avg_id": 49},
    {"_id": 5, "avg_id": 50},
    {"_id": 6, "avg_id": 51},
    {"_id": 7, "avg_id": 52},
    {"_id": 8, "avg_id": 53},
    {"_id": 9, "avg_id": 54},
];

assert.eq(result, expected);

s.stop();
