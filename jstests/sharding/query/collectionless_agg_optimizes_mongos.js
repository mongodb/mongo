/**
 * Tests that collectionless aggregation pipelines get optimized even when running fully on mongos,
 * for both "allPlansExecution" and "executionStats".
 *
 * @tags: [
 *  requires_fcv_81
 * ]
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";

// Verify that $limit gets absorbed by $sort, which indicates that optimization has taken place.
function assertAbsorbedLimit(explain, firstStage) {
    assert(explain.hasOwnProperty('mongos'));
    let mongos = explain['mongos'];
    assert(mongos.hasOwnProperty('stages'));
    let stages = mongos['stages'];
    assert(stages[0].hasOwnProperty(firstStage));
    assert(stages[1].hasOwnProperty('$sort'));
    let sort = stages[1]['$sort'];
    assert(sort.hasOwnProperty('limit'));
}

const st = new ShardingTest({shards: 2, mongos: 1});

let explainCmdResult = st.s.adminCommand({
    aggregate: 1,
    pipeline: [{$queryStats: {}}, {$sort: {keyHash: 1}}, {$limit: 2}],
    explain: true
});
assert.commandWorked(explainCmdResult);
assertAbsorbedLimit(explainCmdResult, '$queryStats');

explainCmdResult = st.s.adminCommand({
    aggregate: 1,
    pipeline: [{$currentOp: {localOps: true}}, {$sort: {shard: 1}}, {$limit: 2}],
    explain: true
});
assert.commandWorked(explainCmdResult);
assertAbsorbedLimit(explainCmdResult, '$currentOp');

explainCmdResult = st.s.adminCommand({
    explain: {
        aggregate: 1,
        pipeline: [{$queryStats: {}}, {$sort: {keyHash: 1}}, {$limit: 2}],
    },
    verbosity: "executionStats",
});
assert.commandWorked(explainCmdResult);
assertAbsorbedLimit(explainCmdResult, '$queryStats');

explainCmdResult = st.s.adminCommand({
    explain: {
        aggregate: 1,
        pipeline: [{$currentOp: {localOps: true}}, {$sort: {shard: 1}}, {$limit: 2}],
    },
    verbosity: "executionStats",
});
assert.commandWorked(explainCmdResult);
assertAbsorbedLimit(explainCmdResult, '$currentOp');

st.stop();
