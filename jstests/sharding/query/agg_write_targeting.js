/*
 * Test that $merge and $out are correctly targeted to a shard that owns data if the output
 * collection is unsplittable.
 *
 * @tags: [
 *   featureFlagTrackUnshardedCollectionsUponCreation,
 *   featureFlagMoveCollection,
 *   multiversion_incompatible,
 *   assumes_balancer_off,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ShardTargetingTest} from "jstests/libs/shard_targeting_util.js";

const kDbName = "agg_write_targeting";

const st = new ShardingTest({shards: 3});
const db = st.s.getDB(kDbName);
const shard0 = st.shard0.shardName;
const shard1 = st.shard1.shardName;
const shard2 = st.shard2.shardName;

assert.commandWorked(db.adminCommand({enableSharding: kDbName, primaryShard: shard0}));

// Note that 'shardDBMap' is left uninitialized in 'shardTargetingTest' because it is only used to
// setup the collections involved in this test.
const shardTargetingTest = new ShardTargetingTest(db, {} /* shardDBMap */);
const kShardedCollName = "sharded";
const kUnsplittable1CollName = "unsplittable_1";
const kUnsplittable2CollName = "unsplittable_2";
const kUnsplittable3CollName = "unsplittable_3";
const kCollDoesNotExistName = "collDoesNotExist";

let shardedColl;
let coll1;
let coll2;
let coll3;

function initCollectionPlacement() {
    if (shardedColl) {
        assert(shardedColl.drop());
    }
    const kShardedCollChunkList = [
        {min: {_id: MinKey}, max: {_id: 3}, shard: shard0},
        {min: {_id: 3}, max: {_id: 6}, shard: shard1},
        {min: {_id: 6}, max: {_id: MaxKey}, shard: shard2}
    ];

    shardTargetingTest.setupColl({
        collName: kShardedCollName,
        collType: "sharded",
        shardKey: {_id: 1},
        chunkList: kShardedCollChunkList,
    });

    if (coll1) {
        assert(coll1.drop());
    }
    shardTargetingTest.setupColl({
        collName: kUnsplittable1CollName,
        collType: "unsplittable",
        owningShard: shard1,
    });

    if (coll2) {
        assert(coll2.drop());
    }
    shardTargetingTest.setupColl({
        collName: kUnsplittable2CollName,
        collType: "unsplittable",
        owningShard: shard2,
    });

    if (coll3) {
        assert(coll3.drop());
    }

    shardTargetingTest.setupColl({
        collName: kUnsplittable3CollName,
        collType: "unsplittable",
        owningShard: shard1,
    });

    shardedColl = db[kShardedCollName];
    coll1 = db[kUnsplittable1CollName];
    coll2 = db[kUnsplittable2CollName];
    coll3 = db[kUnsplittable3CollName];
}

function getExpectedData(collName) {
    const data = [];
    for (let i = 0; i < 10; ++i) {
        data.push({"_id": i, "original_coll": collName});
    }
    return data;
}

function resetData(coll) {
    assert.commandWorked(coll.deleteMany({}));
    assert.commandWorked(coll.insertMany(getExpectedData(coll.getName())));
}

// Function that not only verifies that the write successfully created the documents that we
// expected, but also that they were written to 'expectedShard'.
function assertData(coll, sourceCollName, expectedShard) {
    const data = coll.find({}).sort({_id: 1}).toArray();
    assert.eq(data, getExpectedData(sourceCollName));

    // Connect to 'expectedShard' directly and verify that it has the collection and the contents
    // that we expect it to.
    if (expectedShard) {
        const shardData =
            expectedShard.getDB(kDbName)[coll.getName()].find({}).sort({_id: 1}).toArray();
        assert.eq(data, shardData);
    }
}

// Utility to test the targeting of 'writeStageSpec' when the documents to write originate from a
// $documents stage.
function testDocumentsTargeting(writeStageSpec, expectedShard) {
    const expectedData = getExpectedData("documents");
    const pipeline = [{$documents: expectedData}, writeStageSpec];
    const explain = db.aggregate(pipeline, {explain: true});
    assert.eq(Object.getOwnPropertyNames(explain.shards), [shard1], tojson(explain));

    db.aggregate(pipeline);
    assertData(coll3, "documents", expectedShard);
}

/**
 * Test function which verifies the behavior of a writing aggregate stage. In particular:
 * - 'writingAggSpec' specifies the aggregation writing stage to test.
 * - 'sourceCollName' specifies the name of the collection that the aggregate reads from.
 * - 'destCollName' specifies the name of the collection that the aggregate writes to.
 * - 'destExists' specifies whether the destination collection exists (and if it does, we should
 *   reset its data prior to testing).
 * - 'expectedMergeShardId' allows for optionally specifying which shard we expect to merge on to
 *   test against explain output.
 * - 'expectedShards' allows for optionally specifying a set of shards that we expect to execute on
 *   to test against explain output.
 * - 'expectedDestShard' allows for optionally specifying the shard connection that we expect that
 *   our output collection will exist on. We will directly connect to the shard and see if the
 *   collection exists on it.
 */
function testWritingAgg({
    writingAggSpec,
    sourceCollName,
    destCollName,
    destExists,
    expectedMergeShardId,
    expectedShards,
    expectedDestShard
}) {
    const sourceColl = db[sourceCollName];
    resetData(sourceColl);
    let destColl;
    if (destExists) {
        destColl = db[destCollName];
        resetData(destColl);
    }

    const pipeline = [writingAggSpec];
    const explain = sourceColl.explain().aggregate(pipeline);
    assert.eq(explain.mergeShardId, expectedMergeShardId, tojson(explain));
    assert.eq(
        Object.getOwnPropertyNames(explain.shards).sort(), expectedShards.sort(), tojson(explain));

    sourceColl.aggregate(pipeline);
    destColl = db[destCollName];
    assertData(destColl, sourceColl.getName(), expectedDestShard);
}

/**
 * Utility to test the behavior of a writing aggregate stage which runs concurrently with a
 * 'moveCollection' command.
 */
function testConcurrentWriteAgg(
    {failpointName, writeAggSpec, nameOfCollToMove, expectedDestShard}) {
    let failpoint = configureFailPoint(st.rs1.getPrimary(), failpointName);
    let writingAgg = startParallelShell(
        funWithArgs(function(dbName, sourceCollName, writeAggSpec) {
            assert.commandWorked(db.getSiblingDB(dbName).runCommand(
                {aggregate: sourceCollName, pipeline: [writeAggSpec], cursor: {}}));
        }, kDbName, kUnsplittable1CollName, writeAggSpec), st.s.port);

    failpoint.wait();
    assert.commandWorked(db.adminCommand({moveCollection: nameOfCollToMove, toShard: shard2}));
    failpoint.off();
    writingAgg();

    assertData(coll3, kUnsplittable1CollName, expectedDestShard);
}

function testOut({
    sourceCollName,
    destCollName,
    destExists,
    expectedMergeShardId,
    expectedShards,
    expectedDestShard
}) {
    const outSpec = {$out: destCollName};
    testWritingAgg({
        writingAggSpec: outSpec,
        sourceCollName: sourceCollName,
        destCollName: destCollName,
        destExists: destExists,
        expectedMergeShardId: expectedMergeShardId,
        expectedShards: expectedShards,
        expectedDestShard: expectedDestShard,
    });
}

function testMerge(
    {sourceCollName, destCollName, expectedMergeShardId, expectedShards, expectedDestShard}) {
    const mergeSpec = {$merge: {into: destCollName, on: "_id", whenMatched: "replace"}};
    testWritingAgg({
        writingAggSpec: mergeSpec,
        sourceCollName: sourceCollName,
        destCollName: destCollName,
        destExists:
            true,  // Since $merge performs updates, we always assume that the destination exists.
        expectedMergeShardId: expectedMergeShardId,
        expectedShards: expectedShards,
        expectedDestShard: expectedDestShard,
    });
}

initCollectionPlacement();

// $out tests

// Input and output collection both exist, are both unsharded and both reside on the same
// non-primary shard.
testOut({
    sourceCollName: kUnsplittable1CollName,
    destCollName: kUnsplittable3CollName,
    destExists: true,
    expectedShards: [shard1],
    expectedDestShard: st.shard1,
});

// Input and output collection both exist and are unsharded but reside on different non-primary
// shards.
testOut({
    sourceCollName: kUnsplittable1CollName,
    destCollName: kUnsplittable2CollName,
    destExists: true,
    expectedMergeShardId: shard2,
    expectedShards: [shard1],
    expectedDestShard: st.shard2,
});

// Input collection is sharded. Output collection exists and resides on a non-primary shard.
testOut({
    sourceCollName: kShardedCollName,
    destCollName: kUnsplittable1CollName,
    destExists: true,
    expectedMergeShardId: shard1,
    expectedShards: [shard0, shard1, shard2],
    expectedDestShard: st.shard1,
});

// Output collection does not exist. Input collection is unsharded and resides on a non-primary
// shard. The output collection should be created on the primary shard.
testOut({
    sourceCollName: kUnsplittable1CollName,
    destCollName: kCollDoesNotExistName,
    destExists: false,
    expectedMergeShardId: shard0,
    expectedShards: [shard1],
    expectedDestShard: st.shard0,
});

// Input is not a collection, but $documents, so we should run on the shard that owns output
// collection (if present)
testDocumentsTargeting({$out: coll3.getName()}, st.shard1);

resetData(coll1);
resetData(coll3);

// Output collection exists and resides on a non-primary shard and moved during execution to
// another shard.
testConcurrentWriteAgg({
    failpointName: "hangWhileBuildingDocumentSourceOutBatch",
    writeAggSpec: {$out: kUnsplittable3CollName},
    nameOfCollToMove: coll3.getFullName(),
    expectedDestShard: st.shard1
});

// $merge tests

// Reset our collection placement.
initCollectionPlacement();

// Input and output collections unsharded but reside on two different non-primary shards.
testMerge({
    sourceCollName: kUnsplittable1CollName,
    destCollName: kUnsplittable2CollName,
    expectedMergeShardId: shard2,
    expectedShards: [shard1],
    expectedDestShard: st.shard2,
});

// Input and output collection both exist, are both unsharded and both reside on the same
// non-primary shard.
testMerge({
    sourceCollName: kUnsplittable1CollName,
    destCollName: kUnsplittable3CollName,
    expectedShards: [shard1],
    expectedDestShard: st.shard1,
});

// Input collection sharded, output collection unsharded but not on the primary shard.
testMerge({
    sourceCollName: kShardedCollName,
    destCollName: kUnsplittable1CollName,
    expectedMergeShardId: shard1,
    expectedShards: [shard0, shard1, shard2],
    expectedDestShard: st.shard1,
});

// Input collection unsharded but not on primary shard, output collection sharded.
testMerge({
    sourceCollName: kUnsplittable1CollName,
    destCollName: kShardedCollName,
    expectedShards: [shard1],
});

// Input is not a collection, but $documents, so we should run on the shard that owns output
// collection (if present).
testDocumentsTargeting({$merge: {into: coll3.getName(), on: "_id", whenMatched: "replace"}},
                       st.shard1);

// Reset our collection placement.
initCollectionPlacement();
resetData(coll1);
resetData(coll3);

const concurrentMergeSpec = {
    $merge: {into: kUnsplittable3CollName, on: "_id", whenMatched: "replace"}
};

testConcurrentWriteAgg({
    failpointName: "hangWhileBuildingDocumentSourceMergeBatch",
    writeAggSpec: concurrentMergeSpec,
    nameOfCollToMove: coll1.getFullName(),
    expectedDestShard: st.shard1
});

// Reset our collection placement.
assert.commandWorked(db.adminCommand({moveCollection: coll1.getFullName(), toShard: shard1}));
resetData(coll1);
resetData(coll3);

// Input and output collections unsharded and output collection is moved during execution. During
// execution, update commands should switch over to targeting the inner collection’s new owner or
// the query should fail with QueryPlanKilled.
testConcurrentWriteAgg({
    failpointName: "hangWhileBuildingDocumentSourceMergeBatch",
    writeAggSpec: concurrentMergeSpec,
    nameOfCollToMove: coll3.getFullName(),
    expectedDestShard: st.shard2
});

st.stop();
