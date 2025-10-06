/**
 * Tests for optimizations applied to trivially false predicates in aggregate pipelines.
 * @tags: [
 *   requires_fcv_83,
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
import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {
    getAllNodeExplains,
    getEngine,
    aggPlanHasStage,
    getAggPlanStage,
    getAggPlanStages,
    getWinningPlanFromExplain,
    isEofPlan,
    planHasStage,
} from "jstests/libs/query/analyze_plan.js";

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
    assert(
        planHasStage(db, firstPartPlan, firstPartStage),
        `Expected ${firstPartStage} plan, found ${tojson(firstPartPlan)}`,
    );
    const pipeline = getExplainPipelineFromAggregationResult(plan, {inhibitOptimization: false});
    const unionStages = pipeline.filter((stage) => stage.hasOwnProperty("$unionWith"));
    assert.eq(unionStages.length, 1, "Expected to find only one $unionWith pipeline stage");
    const unionStageWinningPlan = getWinningPlanFromExplain(unionStages[0].$unionWith);
    assert(
        planHasStage(db, unionStageWinningPlan, secondPartStage),
        `Expected ${secondPartStage} plan, found ${tojson(unionStageWinningPlan)}`,
    );
}

const collName = "explain_find_trivially_false_predicates_in_agg_pipelines";

const localCollName = `${collName}-local`;
assertDropAndRecreateCollection(db, localCollName);
const localColl = db[localCollName];
assert.commandWorked(localColl.insert(Array.from({length: 10}, (_, i) => ({a: i, side: "local"}))));

const foreignCollName = `${collName}-foreign`;
assertDropAndRecreateCollection(db, foreignCollName);
const foreignColl = db[foreignCollName];
assert.commandWorked(foreignColl.insert(Array.from({length: 10}, (_, i) => ({b: i, side: "foreign"}))));

jsTestLog(
    "Testing trivially false optimization with $lookup stages. Always false local branch. Inequality lookup condition",
);

let query = [
    {$match: {$alwaysFalse: 1}},
    {
        $lookup: {
            from: foreignCollName,
            let: {a: "$a"},
            pipeline: [{$match: {$expr: {$and: [{$eq: ["$b", "$$a"]}]}}}],
            as: "foreignSide",
        },
    },
];
let explain = localColl.explain().aggregate(query);

let localBranchPlan = getWinningPlanFromExplain(explain);
assert(isEofPlan(db, localBranchPlan));

let queryResults = localColl.aggregate(query).toArray();
assert.eq(queryResults.length, 0, "Expected empty resultset");

jsTestLog(
    "Testing trivially false optimization with $lookup stages. Always false local branch. Equality lookup condition",
);

query = [
    {$match: {$alwaysFalse: 1}},
    {$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "foreignSide"}},
];
explain = localColl.explain().aggregate(query);
jsTestLog(explain);

localBranchPlan = getWinningPlanFromExplain(explain);
assert(isEofPlan(db, localBranchPlan));

queryResults = localColl.aggregate(query).toArray();
assert.eq(queryResults.length, 0, "Expected empty resultset");

jsTestLog(
    "Testing trivially false optimization with $lookup stages. Always false foreign branch. Specifying lookup condition",
);
query = [
    {
        $lookup: {
            from: foreignCollName,
            localField: "a",
            foreignField: "b",
            pipeline: [{$match: {$alwaysFalse: 1}}],
            as: "foreignSide",
        },
    },
];
explain = localColl.explain("executionStats").aggregate(query);

assert(aggPlanHasStage(explain, "$lookup"), `Expected plan to include a $lookup stage ${explain}`);
let planStages = getAggPlanStages(explain, "$lookup");
// In sharded collections we'll get one stage plan for each shard.
assert(planStages.length >= 1, `Expected aggregation pipeline to have one or more $lookup stages: ${explain}`);
planStages.forEach(assertPlanIsEOF);

queryResults = localColl.aggregate(query).toArray();
assert.eq(queryResults.length, 10, `Expected to have one record output for each document from ${localCollName}`);
queryResults.forEach((outputElement) =>
    assert.eq(outputElement.foreignSide.length, 0, "foreign branch is always false. Expected empty nested results"),
);

jsTestLog(
    "Testing trivially false optimization with $lookup stages. Always false foreign branch. Not specified lookup condition",
);
query = [{$lookup: {from: foreignCollName, pipeline: [{$match: {$alwaysFalse: 1}}], as: "foreignSide"}}];
explain = localColl.explain("executionStats").aggregate(query);

assert(aggPlanHasStage(explain, "$lookup"), `Expected aggregation pipeline to have a $lookup stage ${explain}`);
planStages = getAggPlanStages(explain, "$lookup");
// In sharded collections we'll get one stage plan for each shard.
assert(planStages.length >= 1, `Expected plan to include a $lookup stage ${explain}`);
planStages.forEach(assertPlanIsEOF);

queryResults = localColl.aggregate(query).toArray();
assert.eq(queryResults.length, 10, `Expected to have one record output for each document from ${localCollName}`);
queryResults.forEach((outputElement) =>
    assert.eq(outputElement.foreignSide.length, 0, "foreign branch is always false. Expected empty nested results"),
);

jsTestLog("Testing trivially false optimization with $unionWith stages. AlwaysFalse both parts.");
query = [{$match: {$alwaysFalse: 1}}, {$unionWith: {coll: foreignCollName, pipeline: [{$match: {$alwaysFalse: 1}}]}}];
explain = localColl.explain().aggregate(query);
assertUnionOfPlans(explain, "EOF", "EOF");

queryResults = localColl.aggregate(query).toArray();
assert.eq(queryResults.length, 0, "Expected empty resultset since both parts of the union are trivially false");

jsTestLog("Testing trivially false optimization with $unionWith stages. AlwaysFalse first part.");
query = [{$match: {$alwaysFalse: 1}}, {$unionWith: {coll: foreignCollName}}];
explain = localColl.explain().aggregate(query);
assertUnionOfPlans(explain, "EOF", "COLLSCAN");

queryResults = localColl.aggregate(query).toArray();
assert.eq(queryResults.length, 10, `Expected one result doc for every doc on ${foreignCollName} collection`);
queryResults.forEach((outputRecord) =>
    assert.eq(
        outputRecord.side,
        "foreign",
        `All expected documents should be from ${foreignCollName} collection (have side = foreign)`,
    ),
);

jsTestLog("Testing trivially false optimization with $unionWith stages. AlwaysFalse second part.");
query = [{$unionWith: {coll: foreignCollName, pipeline: [{$match: {$alwaysFalse: 1}}]}}];
explain = localColl.explain().aggregate(query);
assertUnionOfPlans(explain, "COLLSCAN", "EOF");

queryResults = localColl.aggregate(query).toArray();
assert.eq(queryResults.length, 10, `Expected one result doc for every doc on ${localCollName} collection`);
queryResults.forEach((outputRecord) =>
    assert.eq(
        outputRecord.side,
        "local",
        `All expected documents should be from ${localCollName} collection (have side = local)`,
    ),
);

jsTestLog(
    "Testing $nor+$alwaysTrue optimization with $lookup stages. Always false local branch. Inequality lookup condition",
);

query = [
    {$match: {$nor: [{$alwaysTrue: 1}]}},
    {
        $lookup: {
            from: foreignCollName,
            let: {a: "$a"},
            pipeline: [{$match: {$expr: {$and: [{$eq: ["$b", "$$a"]}]}}}],
            as: "foreignSide",
        },
    },
];
explain = localColl.explain().aggregate(query);
assert(isEofPlan(db, getWinningPlanFromExplain(explain)));

queryResults = localColl.aggregate(query).toArray();
assert.eq(queryResults.length, 0, "Expected empty resultset");

jsTestLog(
    "Testing $nor+{$nor+$alwaysFalse} optimization with $lookup stages. Always false local branch.Inequality lookup condition ",
);

query = [
    {$match: {$nor: [{$nor: [{$alwaysFalse: 1}]}]}},
    {
        $lookup: {
            from: foreignCollName,
            let: {a: "$a"},
            pipeline: [{$match: {$expr: {$and: [{$eq: ["$b", "$$a"]}]}}}],
            as: "foreignSide",
        },
    },
];
explain = localColl.explain().aggregate(query);
assert(isEofPlan(db, getWinningPlanFromExplain(explain)));

queryResults = localColl.aggregate(query).toArray();
assert.eq(queryResults.length, 0, "Expected empty resultset");

jsTestLog("Testing $nor+$alwaysFalse optimization with $lookup stages.");

query = [
    {$match: {$nor: [{$alwaysFalse: 1}]}},
    {
        $lookup: {
            from: foreignCollName,
            let: {a: "$a"},
            pipeline: [{$match: {$expr: {$and: [{$eq: ["$b", "$$a"]}]}}}],
            as: "foreignSide",
        },
    },
];
explain = localColl.explain().aggregate(query);
let winningPlan = getWinningPlanFromExplain(explain);
assert(!isEofPlan(db, winningPlan));
assert(!winningPlan.filter || bsonWoCompare(winningPlan.filter, {}) == 0);

/**
 * Verify that {$not: some-expression-that-is-always-false-or-true} is optimized
 */

function getMatchFilter(explain) {
    const singleShardExplain = getAllNodeExplains(explain)[0];
    const engine = getEngine(singleShardExplain);
    const matchStageName = engine == "classic" ? "$match" : "MATCH";
    const matchStage = getAggPlanStage(singleShardExplain, matchStageName, true);
    if (engine == "classic") {
        return matchStage["$match"];
    } else {
        return matchStage.filter;
    }
}

function getCollScanFilter(explain) {
    const singleShardExplain = getAllNodeExplains(explain)[0];
    const collScanStage = getAggPlanStage(singleShardExplain, "COLLSCAN", true);
    if (collScanStage.hasOwnProperty("filter") && Object.keys(collScanStage.filter).length !== 0) {
        return collScanStage.filter;
    } else {
        return "NoFilter";
    }
}

// This query is interesting because $addFields and the structure of the query
// require unparsing and parsing it back, and since {$in: []} is optimized to
// $alwaysFalse, this requires parsing {$not: $alwaysFalse}, which fails. If
// {$not: $alwaysFalse} is rewritten to $alwaysTrue, there is no parsing failure
// and the query is simpler.
const q1 = [{$addFields: {t: 0}}, {$match: {$or: [{b: 13, t: 42}], t: {$elemMatch: {$not: {$in: []}}}}}];
let explainResult = localColl.explain().aggregate(q1);
let collScanFilter = getCollScanFilter(explainResult);
assert.docEq(collScanFilter, {b: {$eq: 13}}, "Winning plan filter should match expected filter");

const matchFilter = getMatchFilter(explainResult);
const expectedMatchCondition = {$and: [{t: {$elemMatch: {}}}, {t: {$eq: 42}}]};
assert.docEq(matchFilter, expectedMatchCondition, "$match/MATCH stage should have the expected structure");

// Simple cases
const q2 = [{$match: {t: {$elemMatch: {$not: {$in: []}}}}}];
explainResult = localColl.explain().aggregate(q2);
collScanFilter = getCollScanFilter(explainResult);
assert.docEq(collScanFilter, {t: {"$elemMatch": {}}}, "Winning plan filter should match expected filter");

const q3 = [{$match: {t: {$not: {$in: []}}}}];
explainResult = localColl.explain().aggregate(q3);
collScanFilter = getCollScanFilter(explainResult);
assert.eq(collScanFilter, "NoFilter", "Winning plan should not have a filter property");
