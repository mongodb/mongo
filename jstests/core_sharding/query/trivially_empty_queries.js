//
// Tests that only one shard is targeted for trivially empty queries.
// @tags: [requires_fcv_83, requires_2_or_more_shards]
//

import {getShards} from "jstests/sharding/libs/sharding_util.js";

const shards = getShards(db);
assert.gte(shards.length, 2, "This test requires at least two shards");

const collName = jsTestName();
const coll = db[collName];
const dbName = db.getName();
const ns = dbName + "." + collName;

let shard0 = shards[0];
let shard1 = shards[1];

let shardName0 = shard0._id;
let shardName1 = shard1._id;

coll.drop();
assert.commandWorked(db.adminCommand({shardCollection: ns, key: {"_id": 1}}));
assert.commandWorked(db.adminCommand({moveRange: ns, min: {_id: 0}, max: {_id: 5}, toShard: shardName0}));
assert.commandWorked(db.adminCommand({moveRange: ns, min: {_id: 5}, max: {_id: 10}, toShard: shardName1}));

const res1 = coll.explain().aggregate([{$match: {$expr: false}}]);
assert.eq(Object.keys(res1.shards).length, 1);
const res2 = coll
    .explain()
    .find({a: {$in: []}})
    .finish();
assert.eq(res2.queryPlanner.winningPlan.stage, "SINGLE_SHARD");
