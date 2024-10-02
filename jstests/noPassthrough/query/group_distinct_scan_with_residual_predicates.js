/**
 * Ensures we don't add redundant FETCH stages when planning a $group query that could potentially
 * use a distinct scan. Reproduces BF-35249.
 *
 * @tags: [
 *   featureFlagShardFilteringDistinctScan
 * ]
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 2});

const db = st.getDB("test");
const primaryShard = st.shard0.shardName;
assert.commandWorked(st.s.adminCommand({enableSharding: db.getName(), primaryShard}));

const coll = db[jsTestName()];
assert.commandWorked(st.s.adminCommand({shardCollection: coll.getFullName(), key: {a: 1}}));

const pipeline = [
    {$match: {nss: {$ne: ""}}},
    {$group: {_id: "$nss", placement: {$top: {output: "$$CURRENT", sortBy: {"timestamp": -1}}}}},
    {$match: {_id: {$not: {$regex: /^[^.]+\.system\.resharding\..+$/}}}}
];
assert.commandWorked(st.s.getDB('config').placementHistory.explain().aggregate(pipeline));

st.stop();
