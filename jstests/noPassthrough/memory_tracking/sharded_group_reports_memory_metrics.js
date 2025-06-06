/**
 * Tests that, when the memory tracking feature flag is enabled, memory tracking statistics are
 * reported to the slow query log and explain("executionStats") for aggregations
 * with $group using the classic engine in a sharded cluster.
 *
 */
import {DiscoverTopology} from "jstests/libs/discover_topology.js";
import {runShardedMemoryStatsTest} from "jstests/libs/query/memory_tracking_utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {setParameterOnAllHosts} from "jstests/noPassthrough/libs/server_parameter_helpers.js";

const st = new ShardingTest(Object.assign({shards: 2}));
const testDB = st.s.getDB("test");
setParameterOnAllHosts(DiscoverTopology.findNonConfigNodes(testDB.getMongo()),
                       "internalQueryFrameworkControl",
                       "forceClassicEngine");

const collName = jsTestName();
const coll = testDB[collName];
testDB[collName].drop();

assert.commandWorked(
    testDB.adminCommand({enableSharding: testDB.getName(), primaryShard: st.shard0.shardName}));

st.shardColl(coll, {shard: 1}, {shard: 1}, {shard: 1}, testDB.getName(), true);

// Set up test collection.
assert.commandWorked(coll.insertMany([
    {shard: 1, groupKey: 1, val: "a"},
    {shard: 0, groupKey: 1, val: "b"},
    {shard: 1, groupKey: 2, val: "c"},
    {shard: 0, groupKey: 2, val: "d"},
]));

runShardedMemoryStatsTest({
    db: testDB,
    collName: collName,
    pipeline: [{$group: {_id: "$groupKey", values: {$push: "$val"}}}],
    pipelineComment: "sharded memory stats group test",
    stageName: "group",
    expectedNumGetMores: 2,
    numShards: 2
});
st.stop();
