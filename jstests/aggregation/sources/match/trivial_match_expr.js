// Tests that queries containing trivially true or false expressions are optimized properly.
//
// @tags: [
//   # Tests the 'stages' field of the explain output which is hidden beneath each shard's name when
//   # run against sharded collections.
//   assumes_unsharded_collection,
//   assumes_against_mongod_not_mongos,
//   # Tests the explain output, so does not work when wrapped in a facet.
//   do_not_wrap_aggregations_in_facets,
//   # Explicitly testing optimization.
//   requires_pipeline_optimization,
// ]
import {getWinningPlanFromExplain, planHasStage} from "jstests/libs/query/analyze_plan.js";

const coll = db.trivial_match_expr;
coll.drop();

assert.commandWorked(coll.insert({foo: 123, bar: "123"}));
assert.commandWorked(coll.insert({foo: 0, bar: "0"}));
assert.commandWorked(coll.insert({foo: -1, bar: "-1"}));

function hasFilter(explain) {
    const plan = getWinningPlanFromExplain(explain);
    return "filter" in plan && Object.keys(plan.filter).length > 0;
}

function assertQueryPlanForAggDoesNotContainFilter(stages = [], msg = "") {
    const explain = coll.explain().aggregate(stages);
    assert(!hasFilter(explain), msg + tojson(explain));
}

function assertQueryPlanForAggContainsFilter(stages = [], msg = "") {
    const explain = coll.explain().aggregate(stages);
    assert(hasFilter(explain), msg + tojson(explain));
}

function assertQueryPlanForFindDoesNotContainFilter(query = {}, msg = "") {
    const explain = coll.explain().find(query).finish();
    assert(!hasFilter(explain), msg + tojson(explain));
}

function assertQueryPlanForFindContainsFilter(query = {}, msg = "") {
    const explain = coll.explain().find(query).finish();
    assert(hasFilter(explain), msg + tojson(explain));
}

function assertQueryPlanForFindContainsEOF(query = {}, msg = "") {
    const explain = coll.explain().find(query).finish();
    assert(planHasStage(db, explain, "EOF"), msg + tojson(explain));
    assert(!planHasStage(db, explain, "COLLSCAN")), msg + tojson(explain);
}

function assertQueryPlanForFindContainsExpressIxscan(query = {}, msg = "") {
    const explain = coll.explain().find(query).finish();
    assert(planHasStage(db, explain, "EXPRESS_IXSCAN"), msg + tojson(explain));
    assert(!planHasStage(db, explain, "COLLSCAN")), msg + tojson(explain);
}

assertQueryPlanForAggDoesNotContainFilter([{$match: {$expr: true}}],
                                          "{$expr: true} should be optimized away");

assertQueryPlanForFindDoesNotContainFilter({$expr: true}, "{$expr: true} should be optimized away");

assertQueryPlanForFindContainsEOF({$expr: false}, "{$expr: false} should be optimized away");

assertQueryPlanForFindContainsEOF({$expr: {$eq: ["0", "-1"]}},
                                  "expressions that optimize to false should be optimized away");

assertQueryPlanForFindContainsEOF({$expr: {$const: null}},
                                  "$expr containing falsy constant should be optimized away");

assertQueryPlanForFindContainsEOF({$and: [{$expr: false}, {_id: 15}]},
                                  "$and containing {$expr: false} should be optimized away");

assertQueryPlanForFindContainsEOF({$and: [{$expr: false}, {$expr: true}, {_id: 15}]},
                                  "$and containing {$expr: false} should be optimized away");

assertQueryPlanForFindContainsExpressIxscan(
    {$or: [{$expr: false}, {_id: 15}]},
    "$or expression containing false constant should be optimized to IXSCAN");

assertQueryPlanForFindContainsExpressIxscan(
    {$or: [{$expr: false}, {$expr: false}, {_id: 20}]},
    "$or expression containing false constants should be optimized to IXSCAN");

assertQueryPlanForAggDoesNotContainFilter(
    [{$match: {$expr: "foo"}}],
    "{$expr: 'foo'} contains a truthy constant and should be optimized away");

assertQueryPlanForFindDoesNotContainFilter(
    {$expr: "foo"}, "{$expr: 'foo'} contains a truthy constant and should be optimized away");

assertQueryPlanForAggDoesNotContainFilter(
    [{$match: {$expr: true}}, {$match: {$expr: "foo"}}],
    "Multiple $match stages containing truthy constants should be optimized away");

assertQueryPlanForFindDoesNotContainFilter(
    {$and: [{$expr: true}, {$expr: "foo"}]},
    "$and expression containing multiple truthy constants should be optimized away");

assertQueryPlanForAggContainsFilter(
    [{$match: {$expr: "$foo"}}],
    "{$expr: '$foo'} refers to a field and should not be optimized away");

assertQueryPlanForFindContainsFilter(
    {$expr: "$foo"}, "{$expr: '$foo'} refers to a field and should not be optimized away");

const explainAggregate =
    coll.explain().aggregate([{$match: {$expr: "foo"}}, {$match: {$expr: "$foo"}}]);
assert.eq(getWinningPlanFromExplain(explainAggregate).filter,
          {$expr: "$foo"},
          "$expr with truthy constant expression should be optimized away when used " +
              "in conjunction with $expr containing non-constant expression");

const explainFind = coll.explain().find({$and: [{$expr: "foo"}, {$expr: "$foo"}]}).finish();
assert.eq(getWinningPlanFromExplain(explainFind).filter,
          {$expr: "$foo"},
          "$expr truthy constant expression should be optimized away when used " +
              "in conjunction with $expr containing non-constant expression");
