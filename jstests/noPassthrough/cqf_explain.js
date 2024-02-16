/**
 * Tests the queryPlanner explain output for CQF.
 */

import {
    explainHasOptimizerPhases,
    getAllPlanStages,
    getExplainOptimizerPhases,
    getOptimizer,
    getShardQueryPlans,
    getWinningPlanFromExplain,
    runOnAllTopLevelExplains
} from "jstests/libs/analyze_plan.js"
import {DiscoverTopology} from "jstests/libs/discover_topology.js";
import {
    leftmostLeafStage,
    runWithParamsAllNodes,
    usedBonsaiOptimizer
} from "jstests/libs/optimizer_utils.js";
import {setParameterOnAllHosts} from "jstests/noPassthrough/libs/server_parameter_helpers.js";

let dbName = "test";
let kForwardDir = "forward";
let kBackwardDir = "backward";
let kTryBonsai = "tryBonsai";

// Asserts on the direction of the physical scan in the explain output.
function checkScanDirection(explain, expectedDir) {
    let scan = leftmostLeafStage(explain);

    // Sanity check that the leaf is a physical scan.
    assert.eq(scan.stage, "COLLSCAN");

    // Assert on the direction of the scan.
    assert.eq(scan.direction, expectedDir);
}

function setFrameworkControl(db, value) {
    setParameterOnAllHosts(
        DiscoverTopology.findNonConfigNodes(db.getMongo()), "internalQueryFrameworkControl", value);
}

function analyzeInputStage(inputStage) {
    switch (inputStage.stage) {
        case "ROOT":
            throw "A ROOT stage should not be the input to a stage.";
        case "EVALUATION":
            analyzeEvaluationStage(inputStage);
            break;
        case "FILTER":
            analyzeFilterStage(inputStage);
            break;
        case "COLLSCAN":
            analyzeCollScanStage(inputStage);
            break;
        default:
            throw "Unrecognized input stage: " + inputStage.stage;
    }
}

function analyzeRootStage(rootStage) {
    assert(rootStage.hasOwnProperty("stage"));
    if (rootStage.stage == "EOF") {
        assert(rootStage.hasOwnProperty("planNodeId"));
        assert(!rootStage.hasOwnProperty("inputStage"));
        return;
    }

    assert(rootStage.stage == "ROOT");
    assert(rootStage.hasOwnProperty("projections"));
    assert(rootStage.hasOwnProperty("inputStage"));
    analyzeInputStage(rootStage.inputStage);
}

function analyzeEvaluationStage(evaluationStage) {
    assert(evaluationStage.hasOwnProperty("stage") && evaluationStage.stage == "EVALUATION");
    assert(evaluationStage.hasOwnProperty("planNodeId"));
    assert(evaluationStage.hasOwnProperty("projections"));
    assert(evaluationStage.hasOwnProperty("inputStage"));
    analyzeInputStage(evaluationStage.inputStage);
}

function analyzeFilterStage(filterStage) {
    assert(filterStage.hasOwnProperty("stage") && filterStage.stage == "FILTER");
    assert(filterStage.hasOwnProperty("planNodeId"));
    assert(filterStage.hasOwnProperty("filter"));
    assert(filterStage.hasOwnProperty("inputStage"));
    analyzeInputStage(filterStage.inputStage);
}

function analyzeCollScanStage(collScanStage) {
    assert(collScanStage.hasOwnProperty("stage") && collScanStage.stage == "COLLSCAN");
    assert(collScanStage.hasOwnProperty("planNodeId"));
    assert(collScanStage.hasOwnProperty("direction"));
    assert(collScanStage.hasOwnProperty("projections"));
    assert(!collScanStage.hasOwnProperty("inputStage"));
}

function getAllPlanStagesList(stagesExplain) {
    let stages = getAllPlanStages(stagesExplain);
    let result = [];

    for (let stageObj of stages) {
        result.push(stageObj.stage);
    }

    return result;
}

/**
 * A helper function to perform assertions over an arbitrary amount of explain paths, all of which
 * should have the same stages present.
 *
 * @param {array} explainPathList - a list of explain paths over which we will perform some
 *     assertions
 * @param {array} expectedStages - the list of stages that all given explain paths should contain
 */
function analyzeExplainHelper(explainPathList, expectedStages, expectedDir) {
    // For each of the explan paths given, find the list of stages present in the explain, assert
    // that list is the same as expectedStages, and analyze the root stage of the explain.
    for (let explain of explainPathList) {
        let planStages = getAllPlanStagesList(explain);
        assert.eq(planStages.length, expectedStages.length);

        for (let expectedStage of expectedStages) {
            assert(planStages.includes(expectedStage));
        }

        analyzeRootStage(explain);

        // We can only assert on the scan direction if a COLLSCAN stage is present in the explain.
        // This is not the case for an EOF plan.
        if (!expectedStages.includes("EOF")) {
            checkScanDirection(explain, expectedDir);
        }
    }
}

// Asserts on some of the top-level fields in the find explain.
function analyzeTopLevelExplain(explain, expectedMaxPSRCountReached, expectedParsedQuery) {
    function nodeAssertions(nodeExplain) {
        // Assert that the explain version is 3.
        assert(nodeExplain.explainVersion === "3",
               `Expected the explainVersion field to have value 3 but instead it has value ${
                   nodeExplain.explainVersion}. The whole top-level explain is ${
                   tojson(nodeExplain)}`);

        let path = nodeExplain;
        if (nodeExplain.hasOwnProperty("queryPlanner")) {
            path = nodeExplain.queryPlanner;
        }
        // Assert that CQF is the queryFramework.
        assert(path.queryFramework === "cqf",
               `Expected the queryFramework field to have value "cqf" but instead it has value ${
                   path.queryFramework}. The whole top-level explain is ${tojson(nodeExplain)}`);

        // Assert that the optimizerCounters sub-object is as expected.
        assert(
            path.hasOwnProperty("optimizerCounters"),
            `Explain does not have the "optimizerCounters" field. The whole top-level explain is ${
                tojson(nodeExplain)}`);
        assert(
            path.optimizerCounters.hasOwnProperty("maxPartialSchemaReqCountReached") &&
                path.optimizerCounters.maxPartialSchemaReqCountReached ===
                    expectedMaxPSRCountReached,
            `Expected the optimizerCounters.maxPartialSchemaReqCountReached field to have value ${
                expectedMaxPSRCountReached} but instead it has value ${
                path.optimizerCounters
                    .maxPartialSchemaReqCountReached}. The whole top-level explain is ${
                tojson(nodeExplain)}`);

        // Assert that the parsedQuery sub-object is as expected.
        assert.eq(
            path.parsedQuery,
            expectedParsedQuery,
            `Expected the parsedQuery field to be ${tojson(expectedParsedQuery)} but got ${
                tojson(path.parsedQuery)}. The whole top-level explain is ${tojson(nodeExplain)}.`);

        // Assert that the queryParameters sub-object exists.
        assert(
            path.winningPlan.hasOwnProperty("queryParameters"),
            `Explain does not have the "winningPlan.queryParameters" field. The whole top-level explain is ${
                tojson(nodeExplain)}`);

        // Assert that optimizationTimeMillis is present.
        assert(
            path.hasOwnProperty("optimizationTimeMillis"),
            `Explain does not have the "optimizationTimeMillis" field. The whole top-level explain is ${
                tojson(nodeExplain)}`);
    }

    runOnAllTopLevelExplains(explain, nodeAssertions);
}

// Asserts on the existence of the queryPlannerDebug verbosity in the explain output and its
// corresponding subelements.
function analyzeExistenceOfQueryPlannerDebugVerbosity(explain, isSharded) {
    assert(usedBonsaiOptimizer(explain), tojson(explain));

    if (!isSharded) {
        let optimizerPhases = getExplainOptimizerPhases(explain);

        let phases = [
            "logicalTranslated",
            "logicalStructuralRewrites",
            "logicalMemoSubstitution",
            "physical",
            "physicalLowered"
        ];

        let arrayOfOptimizerPhases = optimizerPhases.map(x => x.name);
        const diff = phases.filter(x => !arrayOfOptimizerPhases.includes(x));
        assert(diff.length === 0,
               "Missing optimizerPhases verbosity phases: " + diff +
                   ".\nThe full explain is: " + tojson(explain));
    }
}

function analyzeExplain(
    explain, expectedStandaloneStages, expectedShardedStages, expectedDir, isSharded) {
    // Sanity check that we used Bonsai.
    assert(usedBonsaiOptimizer(explain), tojson(explain));

    if (isSharded) {
        let allShards = getShardQueryPlans(explain);

        // If we expect an EOF stage in a sharded environment, there will only be one shard in the
        // explain output. Otherwise there will be two.
        assert.eq(expectedShardedStages.includes("EOF") ? 1 : 2, allShards.length);

        analyzeExplainHelper(allShards, expectedShardedStages, expectedDir);
    } else {
        let stagesPath = getWinningPlanFromExplain(explain);
        analyzeExplainHelper([stagesPath], expectedStandaloneStages, expectedDir);
    }
}

function analyzeFindExplain(db,
                            coll,
                            expectedStandaloneStages,
                            expectedShardedStages,
                            expectedDir,
                            isSharded,
                            query,
                            projection = {},
                            hint = {},
                            disableSargableRewrites = true) {
    let explain = runWithParamsAllNodes(
        db,
        [
            {
                key: "internalCascadesOptimizerDisableSargableWhenNoIndexes",
                value: disableSargableRewrites
            },
            {key: "internalCascadesOptimizerEnableParameterization", value: false}
        ],
        () => coll.explain().find(query, projection).hint(hint).finish());
    analyzeExplain(
        explain, expectedStandaloneStages, expectedShardedStages, expectedDir, isSharded);
}

function analyzeAggExplain(db,
                           coll,
                           expectedStandaloneStages,
                           expectedShardedStages,
                           expectedDir,
                           isSharded,
                           pipeline,
                           hint = {},
                           disableSargableRewrites = true) {
    let cmd = {aggregate: coll.getName(), pipeline: pipeline, explain: true};
    if (Object.keys(hint).length > 0) {
        cmd.hint = hint;
    }

    let explain = runWithParamsAllNodes(
        db,
        [
            {
                key: "internalCascadesOptimizerDisableSargableWhenNoIndexes",
                value: disableSargableRewrites
            },
            {key: "internalCascadesOptimizerEnableParameterization", value: false}
        ],
        () => coll.runCommand(cmd));
    analyzeExplain(
        explain, expectedStandaloneStages, expectedShardedStages, expectedDir, isSharded);
}

function runTest(db, coll, isSharded) {
    setFrameworkControl(db, kTryBonsai);

    const emptyColl = db.empty_coll;
    emptyColl.drop();
    // Empty collection should have EOF as explain, find.
    analyzeFindExplain(db,
                       emptyColl,
                       ["EOF"] /* expectedStandaloneStages */,
                       ["EOF"] /* expectedShardedStages */,
                       null /* expectedDir, shouldn't be checked for this case */,
                       isSharded,
                       {} /* query */,
                       {} /* projection */,
                       {} /* hint */);

    // Empty collection should have EOF as explain, agg.
    analyzeAggExplain(db,
                      emptyColl,
                      ["EOF"] /* expectedStandaloneStages */,
                      ["EOF"] /* expectedShardedStages */,
                      null /* expectedDir, shouldn't be checked for this case */,
                      isSharded,
                      [] /* pipeline */,
                      {} /* hint */);

    const contradictionColl = db.contradictionColl;
    contradictionColl.drop();
    // The queries below against this collection will hint a collection scan, so they will go
    // through Bonsai. The index metadata information will tell us that a is non-multikey so the
    // query is a contradiction and will therefore result in an EOF plan. Note that we need to
    // explicitly allow the query to go through saragable rewrites since by default M2 queries do
    // not go through them.
    contradictionColl.insert({a: 10});
    contradictionColl.createIndex({a: 1});

    // Contradiction plan results in EOF plan, find.
    analyzeFindExplain(db,
                       contradictionColl,
                       ["EOF"] /* expectedStandaloneStages */,
                       ["EOF"] /* expectedShardedStages */,
                       null /* expectedDir */,
                       isSharded,
                       {$and: [{a: 2}, {a: 3}]} /* query */,
                       {} /* projection */,
                       {$natural: 1} /* hint */,
                       false /* disableSargableRewrites */);

    // Contradiction plan results in EOF plan, agg.
    analyzeAggExplain(db,
                      contradictionColl,
                      ["EOF"] /* expectedStandaloneStages */,
                      ["EOF"] /* expectedShardedStages */,
                      null /* expectedDir */,
                      isSharded,
                      [{$match: {$and: [{a: 2}, {a: 3}]}}] /* pipeline */,
                      {$natural: 1} /* hint */,
                      false /* disableSargableRewrites */);

    // Hinted forward scan, find.
    analyzeFindExplain(db,
                       coll,
                       ["ROOT", "COLLSCAN"] /* expectedStandaloneStages */,
                       ["ROOT", "FILTER", "COLLSCAN"] /* expectedShardedStages */,
                       kForwardDir /* expectedDir */,
                       isSharded,
                       {} /* query */,
                       {} /* projection */,
                       {$natural: 1} /* hint */);

    // Hinted foward scan, agg.
    analyzeAggExplain(db,
                      coll,
                      ["ROOT", "COLLSCAN"] /* expectedStandaloneStages */,
                      ["ROOT", "FILTER", "COLLSCAN"] /* expectedShardedStages */,
                      kForwardDir /* expectedDir */,
                      isSharded,
                      [] /* pipeline */,
                      {$natural: 1} /* hint */);

    // Hinted backward scan, find.
    analyzeFindExplain(db,
                       coll,
                       ["ROOT", "COLLSCAN"] /* expectedStandaloneStages */,
                       ["ROOT", "FILTER", "COLLSCAN"] /* expectedShardedStages */,
                       kBackwardDir /* expectedDir */,
                       isSharded,
                       {} /* query */,
                       {} /* projection */,
                       {$natural: -1} /* hint */);

    // Hinted backward scan, agg.
    analyzeAggExplain(db,
                      coll,
                      ["ROOT", "COLLSCAN"] /* expectedStandaloneStages */,
                      ["ROOT", "FILTER", "COLLSCAN"] /* expectedShardedStages */,
                      kBackwardDir /* expectedDir */,
                      isSharded,
                      [] /* pipeline */,
                      {$natural: -1} /* hint */);

    // Query that should have more interesting stages in the explain output, find.
    analyzeFindExplain(
        db,
        coll,
        ["ROOT", "EVALUATION", "FILTER", "COLLSCAN"] /* expectedStandaloneStages */,
        ["ROOT", "EVALUATION", "FILTER", "FILTER", "COLLSCAN"] /* expectedShardedStages */,
        kForwardDir /* expectedDir */,
        isSharded,
        {a: {$lt: 5}} /* query */,
        {a: 1} /* projection */,
        {} /* hint */);

    // Query that should have more interesting stages in the explain output, agg.
    analyzeAggExplain(
        db,
        coll,
        ["ROOT", "EVALUATION", "FILTER", "COLLSCAN"] /* expectedStandaloneStages */,
        ["ROOT", "EVALUATION", "FILTER", "FILTER", "COLLSCAN"] /* expectedShardedStages */,
        kForwardDir /* expectedDir */,
        isSharded,
        [{$match: {a: {$lt: 5}}}, {$project: {'a': 1}}] /* pipeline */,
        {} /* hint */);

    // Test that the maxPSRCountReached field is populated as expected: false for the first query
    // where there is only one predicate and true for the second where there are 11 (the current
    // limit is 10). Note that we need to explicitly allow the queries to go through saragable
    // rewrites since by default M2 queries do not go through them.

    let explain = runWithParamsAllNodes(
        db,
        [
            {key: "internalCascadesOptimizerDisableSargableWhenNoIndexes", value: false},
            {key: "internalCascadesOptimizerEnableParameterization", value: false}
        ],
        () => coll.explain().find({$or: [{a: {$lt: 100}}]}).finish());
    analyzeTopLevelExplain(explain,
                           false /* expectedMaxPSRCountReached */,
                           {"filter": {"a": {"$lt": 100}}} /* expectedParsedQuery */);

    // We create an index and use a $natural hint to ensure that the rewrite during which this case
    // could be reached happens.
    assert.commandWorked(coll.createIndex({a: 1}));
    explain = runWithParamsAllNodes(
        db,
        [
            {key: "internalCascadesOptimizerDisableSargableWhenNoIndexes", value: false},
            {key: "internalCascadesOptimizerEnableParameterization", value: false}
        ],
        () => coll.explain()
                  .find({
                      $or: [
                          {a: {$lt: 100}},
                          {b: {$gt: 0}},
                          {c: {$lt: 99}},
                          {d: {$gt: 1}},
                          {e: {$lt: 98}},
                          {f: {$gt: 2}},
                          {g: {$lt: 97}},
                          {h: {$gt: 3}},
                          {i: {$lt: 96}},
                          {j: {$gt: 4}},
                          {k: {$lt: 95}}
                      ]
                  })
                  .hint({$natural: 1})
                  .finish());
    analyzeTopLevelExplain(explain, true /* expectedMaxPSRCountReached */, {
        "filter": {
            "$or": [
                {"a": {"$lt": 100}},
                {"c": {"$lt": 99}},
                {"e": {"$lt": 98}},
                {"g": {"$lt": 97}},
                {"i": {"$lt": 96}},
                {"k": {"$lt": 95}},
                {"b": {"$gt": 0}},
                {"d": {"$gt": 1}},
                {"f": {"$gt": 2}},
                {"h": {"$gt": 3}},
                {"j": {"$gt": 4}}
            ]
        }
    } /* expectedParsedQuery */);
    assert.commandWorked(coll.dropIndex({a: 1}));

    // Test that the parsedQuery field is empty when the query is empty.
    explain = coll.find().explain();
    analyzeTopLevelExplain(
        explain, false /* expectedMaxPSRCountReached */, {"filter": {}} /* expectedParsedQuery */)

    explain = coll.explain().aggregate();
    analyzeTopLevelExplain(
        explain, false /* expectedMaxPSRCountReached */, {"pipeline": []} /* expectedParsedQuery */)

    // Test that the parsedQuery field is correct for a more complex query.
    explain = coll.find({$or: [{a: 1}, {a: {$lt: 1}}]}, {a: 1}).explain();
    analyzeTopLevelExplain(explain, false /* expectedMaxPSRCountReached */, {
        "filter": {"$or": [{"a": {"$eq": 1}}, {"a": {"$lt": 1}}]},
        "projection": {"a": true, "_id": true}
    })

    explain = coll.explain().aggregate([{$match: {$and: [{a: {$lt: 5}}, {a: 5}]}}]);
    analyzeTopLevelExplain(explain,
                           false /* expectedMaxPSRCountReached */,
                           {"pipeline": [{$match: {$and: [{a: {$lt: 5}}, {a: 5}]}}]});

    // Test that the parsedQuery field reveals optimizations done before the query gets translated
    // to ABT.

    // Combine $or of equality predicates into $in.
    explain = coll.find({$or: [{a: 1}, {a: 2}]}).explain();
    analyzeTopLevelExplain(
        explain, false /* expectedMaxPSRCountReached */, {"filter": {"a": {"$in": [1, 2]}}});

    // Reorder stages such that filter comes first.
    explain = coll.explain().aggregate([{$project: {a: 1}}, {$match: {a: 5}}]);
    analyzeTopLevelExplain(
        explain,
        false /* expectedMaxPSRCountReached */,
        {"pipeline": [{$match: {a: 5}}, {$project: {_id: true, a: true}}]},
    );

    // Ensure that, if requested, the queryPlannerDebug verbosity is present correctly in the
    // explain output.
    explain = coll.find({$or: [{a: 1}, {a: 2}]}).explain("queryPlannerDebug");
    analyzeExistenceOfQueryPlannerDebugVerbosity(explain, isSharded);
}

function setup(conn, db, isSharded) {
    const coll = db.explain_cqf;
    coll.drop();

    let docs = [];
    for (let i = 0; i < 100; i++) {
        docs.push({_id: i, a: i});
    }
    coll.insertMany(docs);

    if (isSharded) {
        conn.shardColl(coll.getName(), {_id: 1}, {_id: 50}, {_id: 51});
    }

    return coll;
}

// Standalone
let conn = MongoRunner.runMongod({
    setParameter: {
        featureFlagCommonQueryFramework: true,
        "failpoint.enableExplainInBonsai": tojson({mode: "alwaysOn"}),
    }
});
assert.neq(null, conn, "mongod was unable to start up");
const db = conn.getDB(dbName);
const coll = setup(conn, db, false);
runTest(db, coll, false /* isSharded */);
MongoRunner.stopMongod(conn);

// Sharded
let shardingConn = new ShardingTest({
    shards: 2,
    mongos: 1,
    other: {
        shardOptions: {
            setParameter: {
                "failpoint.enableExplainInBonsai": tojson({mode: "alwaysOn"}),
                featureFlagCommonQueryFramework: true,
            }
        },
        mongosOptions: {
            setParameter: {
                featureFlagCommonQueryFramework: true,
            }
        },
    }
});
const shardedDb = shardingConn.getDB(dbName);
const shardedColl = setup(shardingConn, shardedDb, true);
runTest(shardedDb, shardedColl, true /* isSharded */);
shardingConn.stop();
