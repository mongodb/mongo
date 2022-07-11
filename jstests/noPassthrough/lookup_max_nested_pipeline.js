// This test verifies that a nested $lookup pipeline can have a total combined depth of 20 but no
// greater.
// @tags: [requires_sharding]

(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");       // For assertErrorCode.
load("jstests/libs/collection_drop_recreate.js");  // For assertDropCollection.
load("jstests/libs/discover_topology.js");         // For findNonConfigNodes.

function generateNestedPipeline(foreignCollName, numLevels) {
    let pipeline = [{"$lookup": {pipeline: [], from: foreignCollName, as: "same"}}];

    for (let level = 1; level < numLevels; level++) {
        pipeline = [{"$lookup": {pipeline: pipeline, from: foreignCollName, as: "same"}}];
    }

    return pipeline;
}

function runTest(lookup) {
    const db = null;  // Using the db variable is banned in this function.
    const lookupName = lookup.getName();

    // Deeply nested $lookup pipeline. Confirm that we can execute an aggregation with nested
    // $lookup sub-pipelines up to the maximum depth, but not beyond.
    let nestedPipeline = generateNestedPipeline("lookup", 20);
    assert.commandWorked(
        lookup.getDB().runCommand({aggregate: lookupName, pipeline: nestedPipeline, cursor: {}}));

    nestedPipeline = generateNestedPipeline("lookup", 21);
    assertErrorCode(lookup, nestedPipeline, ErrorCodes.MaxSubPipelineDepthExceeded);

    // Confirm that maximum $lookup sub-pipeline depth is respected when aggregating views whose
    // combined nesting depth exceeds the limit.
    nestedPipeline = generateNestedPipeline(lookupName, 10);

    assertDropCollection(lookup.getDB(), "view1");
    assert.commandWorked(
        lookup.getDB().runCommand({create: "view1", viewOn: lookupName, pipeline: nestedPipeline}));

    nestedPipeline = generateNestedPipeline("view1", 10);
    assertDropCollection(lookup.getDB(), "view2");
    assert.commandWorked(
        lookup.getDB().runCommand({create: "view2", viewOn: "view1", pipeline: nestedPipeline}));

    // Confirm that a composite sub-pipeline depth of 20 is allowed.
    assert.commandWorked(lookup.getDB().runCommand({aggregate: "view2", pipeline: [], cursor: {}}));

    const pipelineWhichExceedsNestingLimit = generateNestedPipeline("view2", 1);
    assertDropCollection(lookup.getDB(), "view3");
    assert.commandWorked(lookup.getDB().runCommand(
        {create: "view3", viewOn: "view2", pipeline: pipelineWhichExceedsNestingLimit}));

    // Confirm that a composite sub-pipeline depth greater than 20 fails.
    assertErrorCode(lookup.getDB().view3, [], ErrorCodes.MaxSubPipelineDepthExceeded);
}

// Run tests on single node.
const standalone = MongoRunner.runMongod();
runTest(standalone.getDB("test").lookup);

MongoRunner.stopMongod(standalone);

// Run tests in a sharded environment. We must set up sharding explicitly, because
// implicitly_shard_accessed_collections.js will attempt to shard views when they are accessed.
const sharded = new ShardingTest({mongos: 1, shards: 2});

assert(sharded.adminCommand({enableSharding: "test"}));
assert(sharded.adminCommand({shardCollection: "test.lookup", key: {_id: 'hashed'}}));

runTest(sharded.getDB('test').lookup);

sharded.stop();
}());
