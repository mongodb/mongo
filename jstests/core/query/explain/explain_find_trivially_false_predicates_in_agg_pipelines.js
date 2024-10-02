/**
 * Tests for optimizations applied to trivially false predicates in aggregate pipelines.
 * @tags: [
 *   requires_fcv_81,
 *   # Tests run here require explaining plans on aggregate commands that could be incomplete
 *   # during stepdown
 *   does_not_support_stepdowns,
 *   # Explain for the aggregate command cannot run within a multi-document transaction
 *   does_not_support_transactions,
 *   # Explain command does not support read concerns other than local
 *   assumes_read_concern_local,
 *   # Tests with balancer also enable 'random_migrations: true' which results in a different
 *   # sharded $lookup plan that cannot be verified whether it has an EOF.
 *   assumes_balancer_off
 * ]
 */

import {getExplainPipelineFromAggregationResult} from "jstests/aggregation/extras/utils.js";
import {
    aggPlanHasStage,
    getAggPlanStages,
    getWinningPlanFromExplain,
    isEofPlan,
    planHasStage
} from "jstests/libs/analyze_plan.js";
import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";

function assertPlanIsEOF(plan) {
    // Explain query output doesn't include planning for the foreign branch hence we use execution
    // stats as a proxy to tell whether the plan was EOF or not. If no docs or keys were examined
    // and no coll was scanned then we can deduce it is because plan was EOF
    assert.eq(plan.totalDocsExamined, 0, `Expected to have 0 docs examined.`);
    assert.eq(plan.totalKeysExamined, 0, `Expected to have 0 keys examined`);
    assert.eq(plan.collectionScans, 0, `Expected to have 0 collection scans`);
}

function assertUnionOfPlans(plan, firstPartStage, secondPartStage) {
    const firstPartPlan = getWinningPlanFromExplain(explain);
    assert(planHasStage(db, firstPartPlan, firstPartStage),
           `Expected ${firstPartStage} plan, found ${tojson(firstPartPlan)}`);
    const pipeline = getExplainPipelineFromAggregationResult(plan, {inhibitOptimization: false});
    const unionStages = pipeline.filter((stage) => stage.hasOwnProperty("$unionWith"));
    assert.eq(unionStages.length, 1, "Expected to find only one $unionWith pipeline stage");
    const unionStageWinningPlan = getWinningPlanFromExplain(unionStages[0].$unionWith);
    assert(planHasStage(db, unionStageWinningPlan, secondPartStage),
           `Expected ${secondPartStage} plan, found ${tojson(unionStageWinningPlan)}`);
}

const collName = "explain_find_trivially_false_predicates_in_agg_pipelines";

const localCollName = `${collName}-local`;
assertDropAndRecreateCollection(db, localCollName);
const localColl = db[localCollName];
assert.commandWorked(localColl.insert(Array.from({length: 10}, (_, i) => ({a: i, side: "local"}))));

const foreignCollName = `${collName}-foreign`;
assertDropAndRecreateCollection(db, foreignCollName);
const foreignColl = db[foreignCollName];
assert.commandWorked(
    foreignColl.insert(Array.from({length: 10}, (_, i) => ({b: i, side: "foreign"}))));

// TODO SERVER-82497. Add test with AlwaysFalse local branch and equality lookup condition.

jsTestLog(
    "Testing trivially false optimization with $lookup stages. Always false local branch. Inequality lookup condition");

let query = [
    { $match: { $alwaysFalse:1 } },
    { $lookup: {
        from: foreignCollName,
        let: { a: "$a" },
        pipeline: [
            { $match:
                { $expr:
                    { $and:
                        [ { $eq: [ "$b", "$$a" ] } ]
                    }
                }
            }
        ],
        as: "foreignSide"
    }}
];
let explain = localColl.explain().aggregate(query);

const localBranchPlan = getWinningPlanFromExplain(explain);
assert(isEofPlan(db, localBranchPlan));

let queryResults = localColl.aggregate(query).toArray();
assert.eq(queryResults.length, 0, "Expected empty resultset");

jsTestLog(
    "Testing trivially false optimization with $lookup stages. Always false foreign branch. Specifying lookup condition");
query = [{$lookup: {from: foreignCollName, localField: "a", foreignField: "b", pipeline: [{$match: {$alwaysFalse: 1}}], as: "foreignSide"}}];
explain = localColl.explain("executionStats").aggregate(query);

assert(aggPlanHasStage(explain, "$lookup"), `Expected plan to include a $lookup stage ${explain}`);
let planStages = getAggPlanStages(explain, "$lookup");
// In sharded collections we'll get one stage plan for each shard.
assert(planStages.length >= 1,
       `Expected aggregation pipeline to have one or more $lookup stages: ${explain}`);
planStages.forEach(assertPlanIsEOF);

queryResults = localColl.aggregate(query).toArray();
assert.eq(queryResults.length,
          10,
          `Expected to have one record output for each document from ${localCollName}`);
queryResults.forEach(
    (outputElement) => assert.eq(outputElement.foreignSide.length,
                                 0,
                                 "foreign branch is always false. Expected empty nested results"));

jsTestLog(
    "Testing trivially false optimization with $lookup stages. Always false foreign branch. Not specified lookup condition");
query = [
    {$lookup: {from: foreignCollName, pipeline: [{$match: {$alwaysFalse: 1}}], as: "foreignSide"}}
];
explain = localColl.explain("executionStats").aggregate(query);

assert(aggPlanHasStage(explain, "$lookup"),
       `Expected aggregation pipeline to have a $lookup stage ${explain}`);
planStages = getAggPlanStages(explain, "$lookup");
// In sharded collections we'll get one stage plan for each shard.
assert(planStages.length >= 1, `Expected plan to include a $lookup stage ${explain}`);
planStages.forEach(assertPlanIsEOF);

queryResults = localColl.aggregate(query).toArray();
assert.eq(queryResults.length,
          10,
          `Expected to have one record output for each document from ${localCollName}`);
queryResults.forEach(
    (outputElement) => assert.eq(outputElement.foreignSide.length,
                                 0,
                                 "foreign branch is always false. Expected empty nested results"));

jsTestLog("Testing trivially false optimization with $unionWith stages. AlwaysFalse both parts.");
query = [
    {$match: {$alwaysFalse: 1}},
    {$unionWith: {coll: foreignCollName, pipeline: [{$match: {$alwaysFalse: 1}}]}}
];
explain = localColl.explain().aggregate(query);
assertUnionOfPlans(explain, "EOF", "EOF");

queryResults = localColl.aggregate(query).toArray();
assert.eq(queryResults.length,
          0,
          "Expected empty resultset since both parts of the union are trivially false");

jsTestLog("Testing trivially false optimization with $unionWith stages. AlwaysFalse first part.");
query = [{$match: {$alwaysFalse: 1}}, {$unionWith: {coll: foreignCollName}}];
explain = localColl.explain().aggregate(query);
assertUnionOfPlans(explain, "EOF", "COLLSCAN");

queryResults = localColl.aggregate(query).toArray();
assert.eq(queryResults.length,
          10,
          `Expected one result doc for every doc on ${foreignCollName} collection`);
queryResults.forEach((outputRecord) =>
                         assert.eq(outputRecord.side,
                                   "foreign",
                                   `All expected documents should be from ${
                                       foreignCollName} collection (have side = foreign)`));

jsTestLog("Testing trivially false optimization with $unionWith stages. AlwaysFalse second part.");
query = [{$unionWith: {coll: foreignCollName, pipeline: [{$match: {$alwaysFalse: 1}}]}}];
explain = localColl.explain().aggregate(query);
assertUnionOfPlans(explain, "COLLSCAN", "EOF");

queryResults = localColl.aggregate(query).toArray();
assert.eq(queryResults.length,
          10,
          `Expected one result doc for every doc on ${localCollName} collection`);
queryResults.forEach(
    (outputRecord) => assert.eq(
        outputRecord.side,
        "local",
        `All expected documents should be from ${localCollName} collection (have side = local)`));

jsTestLog(
    "Testing $nor+$alwaysTrue optimization with $lookup stages. Always false local branch. Inequality lookup condition");

query = [
    { $match: { $nor: [ { $alwaysTrue: 1 } ] } },
    { $lookup: {
        from: foreignCollName,
        let: { a: "$a" },
        pipeline: [
            { $match:
                { $expr:
                    { $and:
                        [ { $eq: [ "$b", "$$a" ] } ]
                    }
                }
            }
        ],
        as: "foreignSide"
    }}
];
explain = localColl.explain().aggregate(query);
assert(isEofPlan(db, getWinningPlanFromExplain(explain)));

queryResults = localColl.aggregate(query).toArray();
assert.eq(queryResults.length, 0, "Expected empty resultset");

jsTestLog(
    "Testing $nor+{$nor+$alwaysFalse} optimization with $lookup stages. Always false local branch.Inequality lookup condition ");

query = [
    { $match: { $nor: [ {$nor: [{$alwaysFalse: 1}]}] }},
    { $lookup: {
        from: foreignCollName,
        let: { a: "$a" },
        pipeline: [
            { $match:
                { $expr:
                    { $and:
                        [ { $eq: [ "$b", "$$a" ] } ]
                    }
                }
            }
        ],
        as: "foreignSide"
    }}
];
explain = localColl.explain().aggregate(query);
assert(isEofPlan(db, getWinningPlanFromExplain(explain)));

queryResults = localColl.aggregate(query).toArray();
assert.eq(queryResults.length, 0, "Expected empty resultset");

jsTestLog("Testing $nor+$alwaysFalse optimization with $lookup stages.");

query = [
    { $match: { $nor: [ { $alwaysFalse: 1 } ] } },
    { $lookup: {
        from: foreignCollName,
        let: { a: "$a" },
        pipeline: [
            { $match:
                { $expr:
                    { $and:
                        [ { $eq: [ "$b", "$$a" ] } ]
                    }
                }
            }
        ],
        as: "foreignSide"
    }}
];
explain = localColl.explain().aggregate(query);
let winningPlan = getWinningPlanFromExplain(explain);
assert(!isEofPlan(db, winningPlan));
assert(!winningPlan.filter || bsonWoCompare(winningPlan.filter, {}) == 0);
