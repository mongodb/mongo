// @tags: [
//   assumes_balancer_off,
//   assumes_unsharded_collection,
//   # Asserts that some queries use a collection scan.
//   assumes_no_implicit_index_creation,
// ]
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {
    assertStagesForExplainOfCommand,
    getWinningPlanFromExplain,
    planHasStage,
} from "jstests/libs/query/analyze_plan.js";

const coll = db.distinct_multikey_index;

coll.drop();
for (let i = 0; i < 10; i++) {
    assert.commandWorked(coll.insert({a: 1, b: 1, geoField: [0, 0], text: "A"}));
    assert.commandWorked(coll.insert({a: 1, b: 2, geoField: [1, 1], text: "B"}));
    assert.commandWorked(coll.insert({a: 2, b: 1, geoField: [1, 2], text: "C"}));
    assert.commandWorked(coll.insert({a: 2, b: 3, geoField: [1, 1], text: "D"}));
}
assert.commandWorked(coll.createIndex({a: 1, b: 1}));

const explain_distinct_with_query = coll.explain("executionStats").distinct("b", {a: 1});
assert.commandWorked(explain_distinct_with_query);
assert(planHasStage(db, getWinningPlanFromExplain(explain_distinct_with_query), "DISTINCT_SCAN"));
assert(planHasStage(db, getWinningPlanFromExplain(explain_distinct_with_query), "PROJECTION_COVERED"));
// If the collection is sharded, we expect at most 2 distinct values per shard. If the
// collection is not sharded, we expect 2 returned.
assert.lte(explain_distinct_with_query.executionStats.nReturned, 2 * FixtureHelpers.numberOfShardsForCollection(coll));

const explain_distinct_without_query = coll.explain("executionStats").distinct("b");
assert.commandWorked(explain_distinct_without_query);
assert(planHasStage(db, getWinningPlanFromExplain(explain_distinct_without_query), "COLLSCAN"));
assert(!planHasStage(db, getWinningPlanFromExplain(explain_distinct_without_query), "DISTINCT_SCAN"));
assert.eq(40, explain_distinct_without_query.executionStats.nReturned);

// Verify that compound special indexes such as '2dsphere' and 'text' can never use index to answer
// 'distinct' command.
const cmdObj = {
    distinct: coll.getName(),
    key: "a",
};
assert.commandWorked(coll.dropIndexes());
assert.commandWorked(coll.createIndex({a: 1, b: 1, geoField: "2dsphere"}));
assertStagesForExplainOfCommand({
    coll: coll,
    cmdObj: cmdObj,
    expectedStages: ["COLLSCAN"],
    stagesNotExpected: ["DISTINCT_SCAN"],
});

assert.commandWorked(coll.dropIndexes());
assert.commandWorked(coll.createIndex({a: 1, b: 1, text: "text"}));

assertStagesForExplainOfCommand({
    coll: coll,
    cmdObj: cmdObj,
    expectedStages: ["COLLSCAN"],
    stagesNotExpected: ["DISTINCT_SCAN"],
});
