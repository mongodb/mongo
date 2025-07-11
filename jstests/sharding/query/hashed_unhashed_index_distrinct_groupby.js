/**
 * Test for a hashed sharded key and a non-hashed index.
 *
 * @tags: [
 *  requires_fcv_82,
 * ]
 */

import {getPlanStages, getWinningPlanFromExplain} from "jstests/libs/query/analyze_plan.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 1});

const shardedDb = st.getDB("test");
st.adminCommand({enablesharding: "test", primaryShard: st.shard0.shardName});
// Shard on {a: 'hashed', m: 1}. Since the collection is empty, an index is automatically made on
// these fields.
assert(st.adminCommand({shardcollection: `test.coll`, key: {a: 'hashed', m: 1}}));

const coll = shardedDb.coll;
assert.commandWorked(coll.insert({m: 0, a: 0}));
assert.commandWorked(coll.createIndex({a: 1, m: 'hashed'}));

function assertIsFetchingAndShardFiltering(explain) {
    const winningPlan = getWinningPlanFromExplain(explain);
    const [distinctScan] = getPlanStages(winningPlan, "DISTINCT_SCAN");
    assert(distinctScan.isFetching);
    assert(distinctScan.isShardFiltering);
}

assert.sameMembers([{_id: 0}], coll.aggregate([{$group: {_id: "$a"}}]).toArray());
assertIsFetchingAndShardFiltering(coll.explain().aggregate([{$group: {_id: "$a"}}]));

assert.sameMembers([0], coll.distinct("a"));
assertIsFetchingAndShardFiltering(coll.explain().distinct("a"));

st.stop();
