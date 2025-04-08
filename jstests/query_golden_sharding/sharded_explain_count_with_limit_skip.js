/**
 * Tests that sharded explain for count commands correctly tracks limit and skip values on the
 * router when targeting multiple shards. Also verifies that nCounted reflects the merged total, not
 * the sum across shards.
 *
 * @tags: [
 *   assumes_read_concern_local,
 *   requires_fcv_82,
 * ]
 */

import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {section} from "jstests/libs/pretty_md.js";
import {outputCountPlanAndResults} from "jstests/libs/query/golden_test_utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({mongos: 1, shards: 2, config: 1});
const db = st.getDB("test");
const coll = assertDropAndRecreateCollection(db, jsTestName());

assert.commandWorked(db.adminCommand({enableSharding: db.getName()}));
assert.commandWorked(db.adminCommand({shardCollection: coll.getFullName(), key: {x: 1}}));

const bulk = coll.initializeUnorderedBulkOp();
for (let i = 0; i < 100; i++) {
    bulk.insert({_id: i, x: i});
}
assert.commandWorked(bulk.execute());

assert.commandWorked(db.adminCommand({split: coll.getFullName(), middle: {x: 50}}));
// Move lower chunk to shard0
assert.commandWorked(
    db.adminCommand({moveChunk: coll.getFullName(), find: {x: 45}, to: st.shard0.shardName}));
// Move upper chunk to shard1
assert.commandWorked(
    db.adminCommand({moveChunk: coll.getFullName(), find: {x: 55}, to: st.shard1.shardName}));

function runCountAndExplain({query, options = {}, expected = {}}) {
    const cmdObj = Object.assign({count: coll.getName(), query}, options);

    const res = db.runCommand(cmdObj);
    assert.commandWorked(res);
    const actualCount = res.n;

    // Run the explain
    const explain = db.runCommand({explain: cmdObj, verbosity: "executionStats"});
    assert.commandWorked(explain);

    outputCountPlanAndResults(cmdObj, explain, expected, actualCount);
}

section("Simple limit targeting multiple shards");
runCountAndExplain({
    query: {x: {$gt: 30}},
    options: {limit: 5},
    expected: {stage: "SHARD_MERGE", limit: 5},
});

section("Simple skip targeting multiple shards");
runCountAndExplain({
    query: {x: {$gt: 45, $lt: 55}},
    options: {skip: 5},
    expected: {stage: "SHARD_MERGE", skip: 5},
});

section("Limit + skip targeting multiple shards");
runCountAndExplain({
    query: {x: {$gt: 30}},
    options: {limit: 5, skip: 5},
    expected: {stage: "SHARD_MERGE", limit: 5, skip: 5},
});

section("nCounted lower than limit");
runCountAndExplain({
    query: {x: {$gte: 49, $lte: 51}},
    options: {limit: 5},
    expected: {stage: "SHARD_MERGE", limit: 5},
});

section("nCounted lower than skip + limit");
runCountAndExplain({
    query: {x: {$gte: 47, $lte: 55}},
    options: {limit: 5, skip: 5},
    expected: {stage: "SHARD_MERGE", limit: 5, skip: 5},
});

section("Simple limit targeting single shard");
runCountAndExplain({
    query: {x: {$gt: 90}},
    options: {limit: 5},
    expected: {stage: "SINGLE_SHARD"},
});

section("Simple skip targeting single shard");
runCountAndExplain({
    query: {x: {$gt: 90}},
    options: {skip: 5},
    expected: {stage: "SINGLE_SHARD"},
});

section("Simple limit + skip targeting single shard");
runCountAndExplain({
    query: {x: {$gt: 80}},
    options: {limit: 5, skip: 5},
    expected: {stage: "SINGLE_SHARD"},
});

st.stop();
