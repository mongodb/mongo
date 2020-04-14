/**
 * Tests to verify that the aggregation pipeline passthrough behaviour works as expected for stages
 * which have sub-pipelines, whose stages may have differing passthrough constraints. This test
 * exercises the fix for SERVER-41290.
 * @tags: [requires_sharding, requires_profiling]
 */
(function() {
'use strict';

load("jstests/libs/profiler.js");  // For profilerHas*OrThrow helper functions.

const st = new ShardingTest({shards: 2});
const mongosDB = st.s0.getDB(jsTestName());
assert.commandWorked(st.s0.adminCommand({enableSharding: jsTestName()}));
st.ensurePrimaryShard(jsTestName(), st.shard0.shardName);
const mongosColl = mongosDB.test;
const mongosOtherColl = mongosDB.otherEmptyCollection;
const primaryShard = st.shard0.getDB(jsTestName());
const shard1DB = st.shard1.getDB(jsTestName());

// Ensure shard0 has the source collection created and is aware of its shard version in order for
// the profiler entry counts to not get offset due to StaleShardVersion
assert.commandWorked(mongosColl.insert({val: 'TestValue'}));

assert.commandWorked(primaryShard.setProfilingLevel(2));
assert.commandWorked(shard1DB.setProfilingLevel(2));

// Verify that the $lookup is passed through to the primary shard when all its sub-pipeline
// stages can be passed through.
let testName = "sub_pipeline_can_be_passed_through";
assert.commandWorked(mongosDB.runCommand({
    aggregate: mongosColl.getName(),
    pipeline:
        [{$lookup: {pipeline: [{$match: {a: "val"}}], from: mongosOtherColl.getName(), as: "c"}}],
    cursor: {},
    comment: testName
}));
profilerHasSingleMatchingEntryOrThrow({
    profileDB: primaryShard,
    filter: {"command.aggregate": mongosColl.getName(), "command.comment": testName}
});
profilerHasZeroMatchingEntriesOrThrow({
    profileDB: shard1DB,
    filter: {"command.aggregate": mongosColl.getName(), "command.comment": testName}
});

// Test to verify that the mongoS doesn't pass the pipeline through to the primary shard when
// $lookup's sub-pipeline has one or more stages which don't allow passthrough. In this
// sub-pipeline, the $merge stage is not allowed to pass through, which forces the pipeline to
// be parsed on mongoS. Since $merge is not allowed within a $lookup, the command thus fails on
// mongoS without ever reaching a shard. This test-case exercises the bug described in
// SERVER-41290.
const pipelineForLookup = [
        {
          $lookup: {
              pipeline: [{$match: {a: "val"}}, {$merge: {into: "merge_collection"}}],
              from: mongosOtherColl.getName(),
              as: "c",
          }
        },
    ];
testName = "lookup_with_merge_cannot_be_passed_through";
assert.commandFailedWithCode(mongosDB.runCommand({
    aggregate: mongosColl.getName(),
    pipeline: pipelineForLookup,
    cursor: {},
    comment: testName
}),
                             51047);
profilerHasZeroMatchingEntriesOrThrow({
    profileDB: primaryShard,
    filter: {"command.aggregate": mongosColl.getName(), "command.comment": testName}
});
profilerHasZeroMatchingEntriesOrThrow({
    profileDB: shard1DB,
    filter: {"command.aggregate": mongosColl.getName(), "command.comment": testName}
});

// Same test as the above with another level of nested $lookup.
const pipelineForNestedLookup = [{
        $lookup: {
            from: mongosOtherColl.getName(),
            as: "field",
            pipeline: [{
                $lookup: {
                    pipeline: [{$match: {a: "val"}}, {$merge: {into: "merge_collection"}}],
                    from: mongosDB.nested.getName(),
                    as: "c",
                }
            }]
        }
    }];
testName = "nested_lookup_with_merge_cannot_be_passed_through";
assert.commandFailedWithCode(mongosDB.runCommand({
    aggregate: mongosColl.getName(),
    pipeline: pipelineForNestedLookup,
    cursor: {},
    comment: testName
}),
                             51047);
profilerHasZeroMatchingEntriesOrThrow({
    profileDB: primaryShard,
    filter: {"command.aggregate": mongosColl.getName(), "command.comment": testName}
});
profilerHasZeroMatchingEntriesOrThrow({
    profileDB: shard1DB,
    filter: {"command.aggregate": mongosColl.getName(), "command.comment": testName}
});

// Test to verify that the mongoS doesn't pass the pipeline through to the primary shard when
// one or more of $facet's sub-pipelines have one or more stages which don't allow passthrough.
// In this sub-pipeline, the $merge stage is not allowed to pass through, which forces the
// pipeline to be parsed on mongoS. Since $merge is not allowed within a $facet, the command
// thus fails on mongoS without ever reaching a shard. This test-case exercises the bug
// described in SERVER-41290.
const pipelineForFacet = [
    {
        $facet: {
            field0: [{$match: {a: "val"}}],
            field1: [{$match: {a: "val"}}, {$merge: {into: "merge_collection"}}],
        }
    },
];
testName = "facet_with_merge_cannot_be_passed_through";
assert.commandFailedWithCode(mongosDB.runCommand({
    aggregate: mongosColl.getName(),
    pipeline: pipelineForFacet,
    cursor: {},
    comment: testName
}),
                             40600);
profilerHasZeroMatchingEntriesOrThrow({
    profileDB: primaryShard,
    filter: {"command.aggregate": mongosColl.getName(), "command.comment": testName}
});
profilerHasZeroMatchingEntriesOrThrow({
    profileDB: shard1DB,
    filter: {"command.aggregate": mongosColl.getName(), "command.comment": testName}
});

// Same test as the above with another level of nested $facet.
const pipelineForNestedFacet = [
    {
        $facet: {
            field0: [{$match: {a: "val"}}],
            field1:
                [{$facet: {field2: [{$match: {a: "val"}}, {$merge: {into: "merge_collection"}}]}}],
        }
    },
];
testName = "facet_with_merge_cannot_be_passed_through";
assert.commandFailedWithCode(mongosDB.runCommand({
    aggregate: mongosColl.getName(),
    pipeline: pipelineForNestedFacet,
    cursor: {},
    comment: testName
}),
                             40600);
profilerHasZeroMatchingEntriesOrThrow({
    profileDB: primaryShard,
    filter: {"command.aggregate": mongosColl.getName(), "command.comment": testName}
});
profilerHasZeroMatchingEntriesOrThrow({
    profileDB: shard1DB,
    filter: {"command.aggregate": mongosColl.getName(), "command.comment": testName}
});

st.stop();
})();
