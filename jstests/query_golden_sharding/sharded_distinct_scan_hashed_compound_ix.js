/**
 * Test for a hashed sharded key and a non-hashed index.
 *
 * @tags: [
 *   requires_fcv_82,
 * ]
 */

import {section} from "jstests/libs/pretty_md.js";
import {
    outputAggregationPlanAndResults,
    outputDistinctPlanAndResults
} from "jstests/libs/query/golden_test_utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 1});

const shardedDb = st.getDB("test");
st.adminCommand({enablesharding: "test", primaryShard: st.shard0.shardName});
// Shard on {a: 'hashed', m: 1}. Since the collection is empty, an index is automatically made on
// these fields.
assert(st.adminCommand({shardcollection: `test.coll`, key: {a: 'hashed', m: 1}}));

const coll = shardedDb[jsTestName()];
assert.commandWorked(coll.insertMany([
    {m: 0, a: 0.0},
    {m: 0.1, a: 0.1},
    {m: 0.2, a: 0.2},
    {m: 0.2, a: 0.9},
    {m: 0.2, a: 1.9},
]));
assert.commandWorked(coll.createIndex({a: 1, m: 'hashed'}));

section('$group by field that is hashed on shard key but not on index');
outputAggregationPlanAndResults(coll, [{$group: {_id: "$a"}}]);

section('distinct() on field that is hashed in shard key but in index');
outputDistinctPlanAndResults(coll, "a");

st.stop();
