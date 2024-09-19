/**
 * Test that $merge works when shard key is missing or null.
 *
 * @tags: [
 *   requires_fcv_81,
 * ]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 2});

const kInputColl = "merge_null_shard_key_input";
const kOutputColl = "merge_null_shard_key_output";

const db = st.s.getDB("test");
const inputColl = db[kInputColl];
const outputColl = db[kOutputColl];

st.shardColl(kOutputColl,
             {"shard.key": 1},  // key
             {"shard.key": 5},  // split
             {"shard.key": 6},  // move
             "test");

assert.commandWorked(inputColl.insertMany(
    [{shard: {key: null}, a: 1}, {shard: {}, b: 2}, {c: 3}, {shard: "scalar", d: 4}]));
assert.doesNotThrow(
    () => inputColl.aggregate([{$addFields: {_id: 1}}, {$merge: {into: outputColl.getName()}}]));
assert.eq(sortDoc(outputColl.findOne({}, {shard: 0})), {_id: 1, a: 1, b: 2, c: 3, d: 4});

st.stop();
