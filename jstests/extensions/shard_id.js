/**
 * Tests that $testShardId correctly propagates the shard ID from the catalog context.
 *
 * - On a standalone or replica set, shardId should be an empty string.
 * - On a shard in a sharded cluster, shardId should be non-empty and match an actual shard name.
 * - On the router (mongos) in a sharded cluster, shardId should be an empty string.
 *
 * @tags: [featureFlagExtensionsAPI]
 */
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

const coll = db[jsTestName()];
coll.drop();
assert.commandWorked(
    coll.insertMany([
        {_id: 0, x: 1},
        {_id: 1, x: 2},
    ]),
);

const isMongos = FixtureHelpers.isMongos(db);
const shardNames = isMongos
    ? db
          .getSiblingDB("config")
          .shards.find()
          .toArray()
          .map((s) => s._id)
    : [];
const isShardedColl = isMongos && db.getSiblingDB("config").collections.countDocuments({_id: coll.getFullName()}) > 0;

function assertShardIdEmpty(results, context) {
    for (const doc of results) {
        assert(doc.hasOwnProperty("shardId"), "Document should have shardId field: " + tojson(doc));
        assert.eq(doc.shardId, "", `shardId should be empty ${context}, got: ` + tojson(doc));
    }
}

function assertShardIdPopulated(results) {
    for (const doc of results) {
        assert(doc.hasOwnProperty("shardId"), "Document should have shardId field: " + tojson(doc));
        assert.contains(doc.shardId, shardNames, "shardId should match a known shard, got: " + doc.shardId);
    }
}

// Test 1: Default mode — stage runs on shards (pushed down by default).
{
    const results = coll.aggregate([{$testShardId: {}}]).toArray();
    assert.eq(results.length, 2, "Expected 2 documents: " + tojson(results));

    if (isMongos) {
        // Stage forwards to shards (getDistributedPlanLogic returns none), so bind() runs on
        // the shard where shardId is populated.
        assertShardIdPopulated(results);
    } else {
        assertShardIdEmpty(results, "on standalone");
    }
}

// Test 2: Router mode — stage runs on mongos via DistributedPlanLogic mergingPipeline.
// Pipeline splitting only occurs for sharded collections. When the collection is unsharded, the
// entire pipeline is sent to the primary shard and the DPL is not consulted.
{
    const results = coll.aggregate([{$testShardId: {runOnRouter: true}}]).toArray();
    assert.eq(results.length, 2, "Expected 2 documents: " + tojson(results));

    if (isShardedColl) {
        // The collection is sharded, so the pipeline is split. The DPL places $testShardId in
        // the merging pipeline, which runs on mongos where shardId is empty.
        assertShardIdEmpty(results, "on router");
    } else if (isMongos) {
        // Unsharded collection in a sharded cluster: the pipeline is sent entirely to the
        // primary shard. The stage runs on the shard, so shardId is non-empty.
        assertShardIdPopulated(results);
    } else {
        assertShardIdEmpty(results, "on standalone");
    }
}
