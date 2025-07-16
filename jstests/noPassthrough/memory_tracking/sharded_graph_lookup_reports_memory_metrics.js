/**
 * Tests that, when the memory tracking feature flag is enabled, memory tracking statistics are
 * reported to the slow query log for aggregations with $graphLookup in a sharded cluster.  Note
 * that explain is not tested here because explain("executionStats") does not report the merging
 * part of a split pipeline and $graphLookup is forced to run on the merging node.
 */
import {runShardedMemoryStatsTest} from "jstests/libs/query/memory_tracking_utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest(Object.assign({shards: 2}));
const testDB = st.s.getDB("test");

const collName = jsTestName();
const coll = testDB[collName];
testDB[collName].drop();

assert.commandWorked(
    testDB.adminCommand({enableSharding: testDB.getName(), primaryShard: st.shard0.shardName}));

st.shardColl(coll, {_id: 1}, {_id: "hashed"});

// Set up test collections.
const docCount = 3;
for (let i = 0; i < docCount; ++i) {
    assert.commandWorked(coll.insertOne({_id: i, to: [i - 1, i, i + 1]}));
}

// Run the $graphLookup stage in a sub-pipeline to force it to run on the router.
let pipeline = [
    {   
        $facet: {
            f: [
                { 
                    $graphLookup: {
                        from: collName,
                        startWith: "$_id",
                        connectFromField: "to",
                        connectToField: "_id",
                        as: "output",
                    }
                },
                {$unwind: "$output"}
            ]
        },
    },
    {$unwind: "$f"},
];

runShardedMemoryStatsTest({
    db: testDB,
    collName: collName,
    commandObj: {
        aggregate: collName,
        pipeline: pipeline,
        cursor: {batchSize: 3},
        comment: "memory stats graphLookup test",
        allowDiskUse: false,
    },
    pipelineComment: "sharded memory stats graphLookup test",
    stageName: "graphLookup",
    expectedNumGetMores: 3,
    numShards: 2,
    skipExplain: true,  // graphLookup will execute on the merging part of the pipeline and will not
                        // appear in the shards' explain output.
    skipInUseMemBytesCheck: true,  // Because we run $graphLookup in a sub-pipeline, we compute the
                                   // result in one shot, and don't have in-use memory hanging
                                   // around between requests.
});
st.stop();
