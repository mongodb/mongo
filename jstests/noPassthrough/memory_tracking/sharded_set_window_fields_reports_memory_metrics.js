/**
 * Tests that, when the memory tracking feature flag is enabled, aggregations with $setWindowFields
 * using the classic engine in a sharded cluster are executed properly. Note that slow query log
 * and explain("executionStats") do not report on the merging part of a split pipeline, and
 * $setWindowFields is forced to be run on the merging node, so memory tracking statistics do not
 * appear in these channels.
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

const pipeline = [{
    $setWindowFields: {
        partitionBy: "$groupKey",
        sortBy: {val: 1},
        output: {values: {$push: "$val", window: {documents: ["unbounded", "current"]}}}
    }
}];

runShardedMemoryStatsTest({
    db: testDB,
    collName: collName,
    commandObj: {
        aggregate: collName,
        pipeline: pipeline,
        cursor: {batchSize: 1},
        comment: "memory stats setWindowFields test",
        allowDiskUse: false,
    },
    pipelineComment: "sharded memory stats setWindowFields test",
    stageName: "_internalSetWindowFields",
    expectedNumGetMores: 2,
    // We don't expect any explain stages on the shards because $setWindowFields is always run on
    // the merging node - so we skip explain.
    skipExplain: true
});
st.stop();
