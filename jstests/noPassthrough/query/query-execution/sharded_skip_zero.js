/**
 * Reproduces BF-41239: tassert when running aggregate([{$skip: 0}]) on a sharded collection
 * when pipeline optimization is skipped.
 *
 * The issue occurs when:
 * 1. A sharded collection has data distributed across multiple shards
 * 2. Pipeline optimization is skipped (e.g., routing table not yet cached, or explicitly disabled)
 * 3. The pipeline [{$skip: 0}] is split, putting $skip: 0 on the merge side
 * 4. convertPipelineToRouterStages() tries to create RouterStageSkip with skip=0
 * 5. RouterStageSkip constructor tasserts because it expects a positive skip value
 *
 * We use the disablePipelineOptimization failpoint to simulate the condition where optimization
 * doesn't run (which normally happens when the routing table isn't available in the cache).
 *
 * @tags: [
 *   requires_sharding,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 2, mongos: 1});

const dbName = "db";
const db = st.getDB(dbName);
const coll = db.getCollection(jsTestName());

st.shardColl(coll, {_id: "hashed"}, {_id: NumberLong(50)}, false, dbName, true /* waitForDelete */);

const bulk = coll.initializeUnorderedBulkOp();
for (let i = 0; i < 100; i++) {
    bulk.insert({_id: i, x: i});
}
assert.commandWorked(bulk.execute());

// Verify data is distributed across both shards
const shardsWithData = st.getShardsForQuery(coll, {});
assert.eq(shardsWithData.length, 2, "Expected documents on both shards");

const disableOptFP = configureFailPoint(st.s, "disablePipelineOptimization");

try {
    const result = coll.aggregate([{$skip: 0}]).toArray();
    assert.eq(result.length, 100, `Expected 100 documents. Found ${result.length}`);
} finally {
    disableOptFP.off();
}

// Also test without failpoint - optimization should remove $skip: 0
const resultOptimized = coll.aggregate([{$skip: 0}]).toArray();
assert.eq(
    resultOptimized.length,
    100,
    `Expected 100 documents with optimization enabled. Found ${resultOptimized.length}`,
);

st.stop();
