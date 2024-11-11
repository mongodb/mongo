// Test query works with IXSCAN in descending scanning direction. These tests ensure shard ids can
// be obtained correctly with descending intervals.

import {
    getPlanStages,
    getWinningPlanFromExplain,
    planHasStage
} from "jstests/libs/query/analyze_plan.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const shardingTest = new ShardingTest({shards: 1});
const db = shardingTest.getDB("test");
const coll = db.shard_key_prefix_with_in_operator;

const shardKey = {
    a: 1,
    b: 1,
    c: 1
};
assert.commandWorked(coll.createIndex(shardKey));

assert.commandWorked(db.adminCommand({enableSharding: db.getName()}));
assert.commandWorked(db.adminCommand({shardCollection: coll.getFullName(), key: shardKey}));

{  // Tests if getShardIdsForQuery() works with descending intervals.
    const query = {a: {$gte: 1, $lte: 2}, b: {$gte: 10, $lte: 20}, c: {$gte: 100, $lte: 200}};
    const sortSpec = {a: -1, b: -1, c: -1};

    // Asserts that the test case contains intervals in descending direction.
    const explain = coll.find(query).sort(sortSpec).explain();
    const winningPlan = getWinningPlanFromExplain(explain.queryPlanner);
    const ixscans = getPlanStages(winningPlan, 'IXSCAN');
    assert.eq(ixscans.length, 1);
    const ixscan = ixscans[0];
    assert.eq(ixscan.direction, "backward");
    assert.eq(ixscan.indexBounds,
              {"a": ["[2.0, 1.0]"], "b": ["[20.0, 10.0]"], "c": ["[200.0, 100.0]"]});

    // Asserts that the command should succeed.
    assert.doesNotThrow(() => coll.find(query).sort(sortSpec).toArray());
}

{  // Tests if getShardIdsForQuery() works with descending index bounds requiring unionizing.
    const query = {a: "foo", b: {$in: ["bar", "baz"]}, c: {$gte: 100, $lte: 200}};
    const sortSpec = {c: -1};

    // Asserts that the test case contains an interval in descending direction for each IXSCAN.
    const explain = coll.find(query).sort(sortSpec).explain();
    const winningPlan = getWinningPlanFromExplain(explain.queryPlanner);
    assert(planHasStage(db, winningPlan, 'SORT_MERGE'));
    const ixscans = getPlanStages(winningPlan, 'IXSCAN');
    assert.eq(ixscans.length, 2);
    assert(ixscans.every(ixscan => ixscan.direction, "backward"));
    assert(ixscans.every(ixscan => ixscan.indexBounds.c, ["[200.0, 100.0]"]));

    // Asserts that the command should succeed.
    assert.doesNotThrow(() => coll.find(query).sort(sortSpec).toArray());
}

shardingTest.stop();
