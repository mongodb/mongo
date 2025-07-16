/**
 * Tests that, when the memory tracking feature flag is enabled, memory tracking statistics are
 * reported to the slow query log for aggregations with $bucketAuto using the classic engine in a
 * sharded cluster.  Note that explain is not tested here because explain("executionStats") does not
 * report the merging part of a split pipeline and $bucketAuto is forced to run on the merging node.
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
const docs = [];
for (let i = 1; i <= 100; i++) {
    docs.push({
        value: i,
        category: i % 10 === 0 ? "decade" : "regular",
        group: Math.floor(i / 25) + 1  // Creates 4 groups (1-25, 26-50, 51-75, 76-100)
    });
}
assert.commandWorked(coll.insertMany(docs));

const pipeline = [{
    $bucketAuto: {
        groupBy: "$value",
        buckets: 5,
        output: {"count": {$sum: 1}, "valueList": {$push: "$value"}, "avgValue": {$avg: "$value"}}
    }
}];

runShardedMemoryStatsTest({
    db: testDB,
    collName: collName,
    commandObj: {
        aggregate: collName,
        pipeline: pipeline,
        cursor: {batchSize: 1},
        comment: "memory stats bucketAuto test",
        allowDiskUse: false,
    },
    pipelineComment: "sharded memory stats bucketAuto test",
    stageName: "bucketAuto",
    expectedNumGetMores: 5,
    numShards: 2,
    skipExplain: true  // $bucketAuto will execute on the merging part of the pipeline and will not
                       // appear in the shards' explain output.
});
st.stop();
