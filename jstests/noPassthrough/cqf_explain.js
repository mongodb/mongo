/**
 * Tests the queryPlanner explain output for CQF.
 */

import {
    getAllPlanStages,
    getShardQueryPlans,
    getWinningPlanFromExplain
} from "jstests/libs/analyze_plan.js"
import {DiscoverTopology} from "jstests/libs/discover_topology.js";
import {leftmostLeafStage, usedBonsaiOptimizer} from "jstests/libs/optimizer_utils.js";
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

function analyzeFindExplain(coll,
                            expectedStandaloneStages,
                            expectedShardedStages,
                            expectedDir,
                            isSharded,
                            query,
                            projection = {},
                            hint = {}) {
    let explain = coll.find(query, projection).hint(hint).explain();
    analyzeExplain(
        explain, expectedStandaloneStages, expectedShardedStages, expectedDir, isSharded);
}

function analyzeAggExplain(coll,
                           expectedStandaloneStages,
                           expectedShardedStages,
                           expectedDir,
                           isSharded,
                           pipeline,
                           hint = {}) {
    let cmd = {aggregate: coll.getName(), pipeline: pipeline, explain: true};
    if (Object.keys(hint).length > 0) {
        cmd.hint = hint;
    }

    let explain = assert.commandWorked(coll.runCommand(cmd));
    analyzeExplain(
        explain, expectedStandaloneStages, expectedShardedStages, expectedDir, isSharded);
}

function runTest(db, coll, isSharded) {
    setFrameworkControl(db, kTryBonsai);

    const emptyColl = db.empty_coll;
    emptyColl.drop();
    // Empty collection should have EOF as explain, find.
    analyzeFindExplain(emptyColl,
                       ["EOF"] /* expectedStandaloneStages */,
                       ["EOF"] /* expectedShardedStages */,
                       null /* expectedDir, shouldn't be checked for this case */,
                       isSharded,
                       {} /* query */,
                       {} /* projection */,
                       {} /* hint */);

    // Empty collection should have EOF as explain, agg.
    analyzeAggExplain(emptyColl,
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
    // query is a contradiction and will therefore result in an EOF plan.
    contradictionColl.insert({a: 10});
    contradictionColl.createIndex({a: 1});

    // Contradiction plan results in EOF plan, find.
    analyzeFindExplain(contradictionColl,
                       ["EOF"] /* expectedStandaloneStages */,
                       ["EOF"] /* expectedShardedStages */,
                       null /* expectedDir */,
                       isSharded,
                       {$and: [{a: 2}, {a: 3}]} /* query */,
                       {} /* projection */,
                       {$natural: 1} /* hint */);

    // Contradiction plan results in EOF plan, agg.
    analyzeAggExplain(contradictionColl,
                      ["EOF"] /* expectedStandaloneStages */,
                      ["EOF"] /* expectedShardedStages */,
                      null /* expectedDir */,
                      isSharded,
                      [{$match: {$and: [{a: 2}, {a: 3}]}}] /* pipeline */,
                      {$natural: 1} /* hint */);

    // Hinted forward scan, find.
    analyzeFindExplain(coll,
                       ["ROOT", "COLLSCAN"] /* expectedStandaloneStages */,
                       ["ROOT", "FILTER", "COLLSCAN"] /* expectedShardedStages */,
                       kForwardDir /* expectedDir */,
                       isSharded,
                       {} /* query */,
                       {} /* projection */,
                       {$natural: 1} /* hint */);

    // Hinted foward scan, agg.
    analyzeAggExplain(coll,
                      ["ROOT", "COLLSCAN"] /* expectedStandaloneStages */,
                      ["ROOT", "FILTER", "COLLSCAN"] /* expectedShardedStages */,
                      kForwardDir /* expectedDir */,
                      isSharded,
                      [] /* pipeline */,
                      {$natural: 1} /* hint */);

    // Hinted backward scan, find.
    analyzeFindExplain(coll,
                       ["ROOT", "COLLSCAN"] /* expectedStandaloneStages */,
                       ["ROOT", "FILTER", "COLLSCAN"] /* expectedShardedStages */,
                       kBackwardDir /* expectedDir */,
                       isSharded,
                       {} /* query */,
                       {} /* projection */,
                       {$natural: -1} /* hint */);

    // Hinted backward scan, agg.
    analyzeAggExplain(coll,
                      ["ROOT", "COLLSCAN"] /* expectedStandaloneStages */,
                      ["ROOT", "FILTER", "COLLSCAN"] /* expectedShardedStages */,
                      kBackwardDir /* expectedDir */,
                      isSharded,
                      [] /* pipeline */,
                      {$natural: -1} /* hint */);

    // Query that should have more interesting stages in the explain output, find.
    analyzeFindExplain(
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
        coll,
        ["ROOT", "EVALUATION", "FILTER", "COLLSCAN"] /* expectedStandaloneStages */,
        ["ROOT", "EVALUATION", "FILTER", "FILTER", "COLLSCAN"] /* expectedShardedStages */,
        kForwardDir /* expectedDir */,
        isSharded,
        [{$match: {a: {$lt: 5}}}, {$project: {'a': 1}}] /* pipeline */,
        {} /* hint */);
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
